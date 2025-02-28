/**
 * Copyright (c) 2021 OceanBase
 * OceanBase Database Proxy(ODP) is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#define USING_LOG_PREFIX PROXY
#include "opsql/func_expr_resolver/ob_func_expr_resolver.h"
#include "opsql/func_expr_parser/ob_func_expr_parser.h"
#include "opsql/func_expr_resolver/proxy_expr/ob_proxy_expr_factory.h"
#include "opsql/func_expr_resolver/proxy_expr/ob_proxy_expr.h"
#include "opsql/expr_resolver/ob_expr_resolver.h"
#include "proxy/route/obproxy_part_info.h"
#include "proxy/route/obproxy_expr_calculator.h"
#include "proxy/mysqllib/ob_proxy_mysql_request.h"
#include "proxy/mysqllib/ob_mysql_request_analyzer.h"
#include "proxy/mysqllib/ob_proxy_session_info.h"
#include "proxy/mysql/ob_prepare_statement_struct.h"
#include "obutils/ob_proxy_sql_parser.h"
#include "utils/ob_proxy_utils.h"
#include "dbconfig/ob_proxy_db_config_info.h"
#include "common/ob_obj_compare.h"

using namespace oceanbase::common;
using namespace oceanbase::obproxy::proxy;
using namespace oceanbase::obproxy::obutils;
using namespace oceanbase::obmysql;
using namespace oceanbase::obproxy::dbconfig;

namespace oceanbase
{
namespace obproxy
{
namespace opsql
{

int64_t ObExprResolverResult::to_string(char *buf, const int64_t buf_len) const
{
  int64_t pos = 0;
  J_OBJ_START();
  for (int64_t i = 0; i < OBPROXY_MAX_PART_LEVEL; ++i) {
    databuff_printf(buf, buf_len, pos, " ranges_[%ld]:", i);

    if (ranges_[i].border_flag_.inclusive_start()) {
      databuff_printf(buf, buf_len, pos, "[");
    } else {
      databuff_printf(buf, buf_len, pos, "(");
    }
    pos += ranges_[i].start_key_.to_plain_string(buf + pos, buf_len - pos);
    databuff_printf(buf, buf_len, pos, " ; ");
    pos += ranges_[i].end_key_.to_plain_string(buf + pos, buf_len - pos);
    if (ranges_[i].border_flag_.inclusive_end()) {
      databuff_printf(buf, buf_len, pos, "]");
    } else {
      databuff_printf(buf, buf_len, pos, ")");
    }

    databuff_printf(buf, buf_len, pos, ",");
  }
  J_OBJ_END();
  return pos;
}

/**
 * @brief The range's border flag is decided by the last valid column.
 *        If use range directly, the range will cover partition which
 *        doesn't contain the data.
 * 
 * @param range
 * @param border_flags all columns' border flag
 * @return int 
 */
int ObExprResolver::preprocess_range(ObNewRange &range, ObIArray<ObBorderFlag> &border_flags) {
  int ret = OB_SUCCESS;
  ObObj *obj_start = const_cast<ObObj*>(range.start_key_.get_obj_ptr());
  ObObj *obj_end = const_cast<ObObj*>(range.end_key_.get_obj_ptr());
  int64_t invalid_idx = range.start_key_.get_obj_cnt();
  for (int64_t i = 0; i < range.start_key_.get_obj_cnt(); i++) {
    // find the last valid col, 
    // use the last valid col's border flag as range's border flag
    range.border_flag_.set_data(border_flags.at(i).get_data());
    if (OB_ISNULL(obj_start) || OB_ISNULL(obj_end)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected null pointer");
    } else {
      ObCompareCtx cmp_ctx(ObMaxType, CS_TYPE_INVALID, true, INVALID_TZ_OFF);
      bool need_cast = false;
      ObObj cmp_result(false);
      if (OB_FAIL(ObObjCmpFuncs::compare(cmp_result, *obj_start, *obj_end, cmp_ctx, ObCmpOp::CO_EQ, need_cast))) {
        LOG_WARN("fail to compare", K(ret));
        invalid_idx = i + 1;
        ret = OB_SUCCESS;
        break;
      } else if (!cmp_result.get_bool()) {
        invalid_idx = i + 1;
        break;
      }
    }
  }
  // set the cols after invalid_idx(included) to (max : min) 
  for (int64_t i = invalid_idx; i < range.start_key_.get_obj_cnt(); i++) {
    (obj_start + i)->set_max_value();
    (obj_end + i)->set_min_value();
  }
  LOG_DEBUG("succ to simplify range", K(range));
  return ret;
}

int ObExprResolver::resolve(ObExprResolverContext &ctx, ObExprResolverResult &result)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(ctx.relation_info_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid ctx", K(ctx.relation_info_), K(ret));
  } else {
    proxy::ObProxyPartInfo *part_info = ctx.part_info_;

    // to store partition column's border flag as order above like
    // "where c2 > 1 and c1 < 33" => ordered_part_col_border [(exclusive_start, exclusive_end]
    ObSEArray<ObBorderFlag, 2> part_columns_border;
    ObSEArray<ObBorderFlag, 2> sub_part_columns_border;

    // init range and border
    if (part_info->get_part_level() >= share::schema::PARTITION_LEVEL_ONE) {
      if (OB_FAIL(result.ranges_[0].build_row_key(part_info->get_part_columns().count(), allocator_))) {
        LOG_WARN("fail to init range", K(ret));
      } else {
        for (int i = 0; OB_SUCC(ret) && i < part_info->get_part_columns().count(); i++) {
          if (OB_FAIL(part_columns_border.push_back(ObBorderFlag()))) {
            LOG_WARN("fail to push border flag", K(i), K(ret));
          }
        }
      }
    }
    if (part_info->get_part_level() == share::schema::PARTITION_LEVEL_TWO) {
      if (OB_FAIL(result.ranges_[1].build_row_key(part_info->get_sub_part_columns().count(), allocator_))) {
        LOG_WARN("fail to init range", K(ret));
      } else {
        for (int i = 0; OB_SUCC(ret) && i < part_info->get_sub_part_columns().count(); i++) {
          if (OB_FAIL(sub_part_columns_border.push_back(ObBorderFlag()))) {
            LOG_WARN("fail to push border flag", K(i), K(ret));
          }
        }
      }
    }

    if (OB_SUCC(ret)) {
      ObObj *target_obj = NULL;
      void *tmp_buf = NULL;

      // ignore ret in for loop
      // resolve every relation
      for (int64_t i = 0; i < ctx.relation_info_->relation_num_; ++i) {
        if (OB_ISNULL(ctx.relation_info_->relations_[i])) {
          LOG_INFO("relations is not valid here, ignore it",
                   K(ctx.relation_info_->relations_[i]), K(i));
        } else if (OB_UNLIKELY(ctx.relation_info_->relations_[i]->level_ == PART_KEY_LEVEL_ZERO)) {
          LOG_INFO("level is zero, ignore it");
        } else {
          if (OB_ISNULL(tmp_buf = allocator_.alloc(sizeof(ObObj)))) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
            LOG_WARN("fail to alloc new obj", K(ret));
          } else if (OB_ISNULL(target_obj = new (tmp_buf) ObObj())) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("fail to do placement new", K(ret));
          } else if (OB_FAIL(resolve_token_list(ctx.relation_info_->relations_[i], ctx.part_info_, ctx.client_request_,
                                                ctx.client_info_, ctx.ps_id_entry_, ctx.text_ps_entry_, target_obj,
                                                ctx.sql_field_result_))) {
            LOG_INFO("fail to resolve token list, ignore it", K(ret));
          } else if ((PART_KEY_LEVEL_ONE == ctx.relation_info_->relations_[i]->level_ ||
                      PART_KEY_LEVEL_BOTH == ctx.relation_info_->relations_[i]->level_) &&
                     OB_FAIL(place_obj_to_range(ctx.relation_info_->relations_[i]->type_, 
                                                ctx.relation_info_->relations_[i]->first_part_column_idx_,
                                                target_obj, &result.ranges_[0], part_columns_border))) {
            LOG_WARN("fail to place obj to range of part level one", K(ret));
          } else if ((PART_KEY_LEVEL_TWO == ctx.relation_info_->relations_[i]->level_ ||
                      PART_KEY_LEVEL_BOTH == ctx.relation_info_->relations_[i]->level_) && 
                     OB_FAIL(place_obj_to_range(ctx.relation_info_->relations_[i]->type_, 
                                                ctx.relation_info_->relations_[i]->second_part_column_idx_,
                                                target_obj, &result.ranges_[1], sub_part_columns_border))) {
            LOG_WARN("fail to place obj to range of part level two", K(ret));                      
          } else {}
          if (OB_NOT_NULL(target_obj)) {
            allocator_.free(target_obj);
          }
        }
      }

      if (OB_SUCC(ret) && ctx.is_insert_stm_) {
        if(OB_FAIL(handle_default_value(ctx.parse_result_->part_key_info_,
                                        ctx.client_info_, result.ranges_,
                                        ctx.sql_field_result_,part_columns_border,
                                        sub_part_columns_border, part_info->is_oracle_mode()))){
          LOG_WARN("fail to handle default value of part keys", K(ret));
        }
      }

      if (OB_SUCC(ret)) {
        if (OB_FAIL(preprocess_range(result.ranges_[0], part_columns_border))) {
          LOG_WARN("fail to preprocess range, part key level 0", K(ret));
        } else if (OB_FAIL(preprocess_range(result.ranges_[1], sub_part_columns_border))) {
          LOG_WARN("fail to preprocess range, part key level 1", K(ret));
        } else {}
      }
    }
  }
  return ret;
}

/**
 * @brief 1. Place the resolved partition obj to range.start_key_/range.end_key_[idx].
 *        The idx is the obj's according column's idx in partition expression.
 *        2. Set the border flag of the partition obj. Use the border flag later in ObExprResolver::preprocess_range. 
 * 
 * @param relation 
 * @param part_info 
 * @param target_obj
 * @param range        out
 * @param border_flags out
 * @param part_columns 
 * @return int 
 */
int ObExprResolver::place_obj_to_range(ObProxyFunctionType type,
                                       int64_t idx_in_part_columns,
                                       ObObj *target_obj,
                                       ObNewRange *range,
                                       ObIArray<ObBorderFlag> &border_flags)
{
  int ret = OB_SUCCESS; 
  ObObj *start_obj = const_cast<ObObj*>(range->start_key_.get_obj_ptr()) + idx_in_part_columns;
  ObObj *end_obj = const_cast<ObObj*>(range->end_key_.get_obj_ptr()) + idx_in_part_columns;
  if (OB_ISNULL(start_obj) || OB_ISNULL(end_obj) || OB_ISNULL(target_obj)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected null pointer");
  }

  // 1. deep copy the resolved obj to right position in result.ranges_[x]
  // 2. set the resolved obj's border flag
  if (OB_SUCC(ret)) {
    switch (type) {
     case F_COMP_EQ:
       *start_obj = *target_obj;
       *end_obj = *target_obj;
        border_flags.at(idx_in_part_columns).set_inclusive_start();
        border_flags.at(idx_in_part_columns).set_inclusive_end();
       break;
     case F_COMP_GE:
       *start_obj = *target_obj;
       border_flags.at(idx_in_part_columns).set_inclusive_start();
       break;
     case F_COMP_GT:
       *start_obj = *target_obj;
       break;
     case F_COMP_LE:
       *end_obj = *target_obj;
       border_flags.at(idx_in_part_columns).set_inclusive_end();
       break;
     case F_COMP_LT:
       *end_obj = *target_obj;
       break;
     default:
       LOG_INFO("this func is not useful for range", "func_type",
                 get_obproxy_function_type(type));
       break;
    } // end of switch
  }
  return ret;
}

/*
 * calculate partition key value
 * for normal ps sql, placeholder_idx_ in token node means the pos of '?'
 * for normal pl sql, placeholder_idx_ in token node means the index of call_info.params_
 * for pl sql with ps, placeholder_idx_ in call_info_node_ means the pos of '?'
 * for example: ps sql = call func1(11, ?, 22, ?),
 * the first sql of func1 is select * from t1 where a = :1 and b = :2 and c =:3 and d = :4
 * result:
 * call_info_.params_[1].placeholder_idx_ = 0, call_info_.params_[3].placeholder_idx_ = 1
*/
int ObExprResolver::resolve_token_list(ObProxyRelationExpr *relation,
                                       ObProxyPartInfo *part_info,
                                       ObProxyMysqlRequest *client_request,
                                       ObClientSessionInfo *client_info,
                                       ObPsIdEntry *ps_id_entry,
                                       ObTextPsEntry *text_ps_entry,
                                       ObObj *target_obj,
                                       SqlFieldResult *sql_field_result,
                                       const bool has_rowid)
{
  int ret = OB_SUCCESS;
  UNUSED(text_ps_entry);
  if (OB_ISNULL(target_obj)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("target_obj is null");
  } else if (OB_ISNULL(relation) || OB_ISNULL(part_info)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_INFO("relation or part info is null", K(relation), K(part_info), K(ret));
  } else if (OB_ISNULL(relation->right_value_) || OB_ISNULL(relation->right_value_->head_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_INFO("token list or head is null", K(relation->right_value_), K(ret));
  } else {
   ObProxyTokenNode *token = relation->right_value_->head_;
   int64_t col_idx = relation->column_idx_;
   if (TOKEN_STR_VAL == token->type_) {
     target_obj->set_varchar(token->str_value_.str_, token->str_value_.str_len_);
     target_obj->set_collation_type(ObCharset::get_default_collation(ObCharset::get_default_charset()));
   } else if (TOKEN_INT_VAL == token->type_) {
     target_obj->set_int(token->int_value_);
   } else if (TOKEN_PLACE_HOLDER == token->type_) {
     int64_t param_index = token->placeholder_idx_;
     if (OB_FAIL(get_obj_with_param(*target_obj, client_request, client_info,
                                    part_info, ps_id_entry, param_index))) {
       LOG_DEBUG("fail to get target obj with param", K(ret));
     }
   } else if (TOKEN_FUNC == token->type_) {
     if (OB_FAIL(calc_token_func_obj(token, client_info, *target_obj, sql_field_result, part_info->is_oracle_mode()))) {
       LOG_WARN("fail to calc token func obj", K(ret));
     }
   } else {
     ret = OB_INVALID_ARGUMENT;
   }

   if (OB_SUCC(ret)
       && !has_rowid
       && part_info->has_generated_key()) {
     int64_t target_idx = -1;
     ObProxyPartKeyInfo &part_key_info = part_info->get_part_key_info();
     if (col_idx >= part_key_info.key_num_) {
       ret = OB_ERR_UNEXPECTED;
       LOG_WARN("relation column index is invalid", K(col_idx), K(part_key_info.key_num_), K(ret));
     } else if (part_key_info.part_keys_[col_idx].is_generated_) {
       // do nothing, user sql explicitly contains value for generated key, no need to calculate
     } else if (FALSE_IT(target_idx = part_key_info.part_keys_[col_idx].generated_col_idx_)) {
       // will not come here
     } else if (OB_UNLIKELY(0 > target_idx)) {
       LOG_DEBUG("this relation's part key is not used to generate column");
     } else if (OB_UNLIKELY(target_idx >= part_key_info.key_num_)
                || OB_UNLIKELY(!part_key_info.part_keys_[target_idx].is_generated_)
                || OB_UNLIKELY(part_key_info.part_keys_[target_idx].level_ != relation->level_)) {
       ret = OB_ENTRY_NOT_EXIST;
       LOG_WARN("fail to get generated key value, source key is not offered",
                K(col_idx), K(part_key_info.key_num_), K(target_idx), K(ret));
     } else if (OB_FAIL(calc_generated_key_value(*target_obj, part_key_info.part_keys_[col_idx],
                        part_info->is_oracle_mode()))) {
       LOG_WARN("fail to get generated key value", K(target_obj), K(ret));
     } else {
       LOG_DEBUG("succ to calculate generated key value", K(target_obj), K(ret));
     }
   }

   if (OB_SUCC(ret) && ObStringTC == target_obj->get_type_class()) {
     // The character set of the string parsed from the parser uses the value of the variable collation_connection
     target_obj->set_collation_type(static_cast<common::ObCollationType>(client_info->get_collation_connection()));
   }
  } // end of else
  return ret;
}

/*
 * calculate func token, convert token node to param node to reuse func resolver
 */
int ObExprResolver::calc_token_func_obj(ObProxyTokenNode *token,
                                        ObClientSessionInfo *client_session_info,
                                        ObObj &target_obj,
                                        SqlFieldResult *sql_field_result,
                                        const bool is_oracle_mode)
{
  int ret = OB_SUCCESS;
  ObProxyParamNode *param_node = NULL;
  ObProxyExprFactory factory(allocator_);
  ObFuncExprResolverContext ctx(&allocator_, &factory);
  ObFuncExprResolver resolver(ctx);
  ObProxyExpr *expr;

  if (OB_FAIL(convert_token_node_to_param_node(token, param_node))) {
    LOG_WARN("fail to convert func token to param node", K(ret));
  } else if (OB_ISNULL(param_node)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("fail to convert func token to param node", K(ret));
  } else if (OB_FAIL(resolver.resolve(param_node, expr))) {
    LOG_WARN("proxy expr resolve failed", K(ret));
  } else {
    ObSEArray<ObObj, 4> result_array;
    ObProxyExprCalcItem calc_item(const_cast<SqlFieldResult *>(sql_field_result));
    ObProxyExprCtx expr_ctx(0, TESTLOAD_NON, false, &allocator_, client_session_info);
    expr_ctx.is_oracle_mode = is_oracle_mode;
    if (OB_FAIL(expr->calc(expr_ctx, calc_item, result_array))) {
      LOG_WARN("calc expr result failed", K(ret));
    } else if (OB_FAIL(result_array.at(0, target_obj))) {
      LOG_WARN("get expr calc result fail", K(ret));
    }
  }

  return ret;
}

int ObExprResolver::handle_default_value(ObProxyPartKeyInfo &part_key_info,
                                         proxy::ObClientSessionInfo *client_info,
                                         common::ObNewRange ranges[],
                                         obutils::SqlFieldResult *sql_field_result,
                                         ObIArray<ObBorderFlag> &part_border_flags,
                                         ObIArray<ObBorderFlag> &sub_part_border_flags,
                                         bool is_oracle_mode)
{
  int ret = OB_SUCCESS;
  ObObj *target_obj = NULL;
  void *tmp_buf = NULL;
  for (int i = 0;  OB_SUCC(ret) && i < part_key_info.key_num_; i++) {
    ObProxyPartKeyLevel level = part_key_info.part_keys_[i].level_;
    int64_t column_idx = part_key_info.part_keys_[i].idx_in_part_columns_;
    bool is_need_default_val = false;

    if (OB_ISNULL(tmp_buf = allocator_.alloc(sizeof(ObObj)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("fail to alloc new obj", K(ret));
    } else if (OB_ISNULL(target_obj = new (tmp_buf) ObObj())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("fail to do placement new ObObj", K(ret));
    }

    if(OB_SUCC(ret)){
      if (part_key_info.part_keys_[i].is_generated_ && !part_key_info.part_keys_[i].is_exist_in_sql_) {
        int source_idx = -1;
        ObProxyParseString *var_str = NULL;
        for (int j = 0; j < part_key_info.key_num_; j++) {
          if (part_key_info.part_keys_[j].generated_col_idx_ == i
              && !part_key_info.part_keys_[j].is_exist_in_sql_) {
            source_idx = j;
            break;
          }
        }
        if (source_idx >= 0) {
          int64_t real_source_idx = part_key_info.part_keys_[source_idx].real_source_idx_;
          if (real_source_idx < 0 || real_source_idx > OBPROXY_MAX_PART_KEY_NUM){
            // invalid real source idx
          } else if (FALSE_IT(var_str = &part_key_info.part_keys_[real_source_idx].default_value_) || var_str->str_len_ < 0 ){
          } else if (OB_FAIL(parse_and_resolve_default_value(*var_str, client_info, sql_field_result, target_obj, is_oracle_mode))){
            LOG_WARN("parse and resolve default value of partition key failed", K(ret));
          } else if (OB_FAIL(calc_generated_key_value(*target_obj, part_key_info.part_keys_[source_idx], is_oracle_mode))) {
            LOG_WARN("fail to get generated key value", K(target_obj), K(ret));
          } else {
            is_need_default_val = true;
          }
        }
      } else if (!part_key_info.part_keys_[i].is_exist_in_sql_) {
        ObProxyParseString &var_str = part_key_info.part_keys_[i].default_value_;
        if (var_str.str_len_ > 0) {
          if (OB_FAIL(parse_and_resolve_default_value(var_str, client_info, sql_field_result, target_obj, is_oracle_mode))) {
            LOG_WARN("parse and resolve default value of partition key failed", K(ret));
          } else {
            is_need_default_val = true;
          }
        }
      }
    }
    
    if (OB_SUCC(ret) && is_need_default_val) {
      if ((PART_KEY_LEVEL_ONE == level || PART_KEY_LEVEL_BOTH == level) &&
           OB_FAIL(place_obj_to_range(F_COMP_EQ, column_idx, target_obj, &ranges[0], part_border_flags))) {
        LOG_WARN("fail to place obj to range of part level one", K(ret));
      } else if ((PART_KEY_LEVEL_TWO == level || PART_KEY_LEVEL_BOTH == level) &&
                  OB_FAIL(place_obj_to_range(F_COMP_EQ, column_idx, target_obj, &ranges[1], sub_part_border_flags))) {
        LOG_WARN("fail to place obj to range of part level two", K(ret));
      } else {}
    }
    if (OB_NOT_NULL(target_obj)) {
      allocator_.free(target_obj);
    }
  }
  return ret;
}

int ObExprResolver::parse_and_resolve_default_value(ObProxyParseString &default_value,
                                                    ObClientSessionInfo *client_session_info,
                                                    SqlFieldResult *sql_field_result,
                                                    ObObj *target_obj,
                                                    bool is_oracle_mode)
{
  int ret = OB_SUCCESS;
  number::ObNumber nb;
  if (default_value.str_len_ > 0 ){
    int64_t tmp_pos = 0;
    if (OB_FAIL(target_obj->deserialize(default_value.str_ , default_value.str_len_, tmp_pos))) {
      LOG_WARN("fail to deserialize default value of part key");
    } else if (FALSE_IT(LOG_DEBUG("default value deserialize succ" , K(*target_obj)))) {
    } else if (target_obj->is_varchar() && is_oracle_mode) { 
      // oracle mode return the default as a unresolved varchar type obj 
      ObString default_value_expr = target_obj->get_varchar();
      if (default_value_expr.empty()) {
        target_obj->set_varchar(ObString());
      } else if ('\'' == default_value_expr[0]) {
        // match string type
        ObString dst;
        if (2 >= default_value_expr.length()) {
          dst = ObString();
          // remove single quotes
        } else if (OB_FAIL(ob_sub_str(allocator_, default_value_expr, 1, default_value_expr.length() - 2, dst))) {
          LOG_WARN("get sub stirng of default value failed", K(ret));
        }
        if (OB_SUCC(ret)) {
          target_obj->set_varchar(dst);
        }
      } else if ( OB_SUCCESS == nb.from(default_value_expr.ptr(), default_value_expr.length(), allocator_)) {
        // match positive number
      } else if ('-' == default_value_expr[0] && 4 <= default_value_expr.length()) {
        // match megative number
        // server return negative with brackets, -1 -> -(1)
        ObString number_part;
        char *buf = NULL;
        if(OB_FAIL(ob_sub_str(allocator_, default_value_expr, 2, default_value_expr.length() - 2, number_part))) {
          LOG_WARN("get default value number part fail", K(ret));
        } else if (OB_ISNULL(buf = static_cast<char *>(allocator_.alloc(number_part.length() + 1)))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
          LOG_WARN("alloc memory failed", K(ret));
        } else {
          MEMCPY(buf, "-", 1);
          MEMCPY(buf + 1, number_part.ptr(), number_part.length());
          target_obj->set_varchar(ObString(number_part.length()+1, buf));
        }
      } else {
        // match expr
        ObFuncExprParser parser(allocator_, SHARDING_EXPR_FUNC_PARSE_MODE);
        ObFuncExprParseResult result;

        ObProxyExprFactory factory(allocator_);
        ObFuncExprResolverContext ctx(&allocator_, &factory);
        ObFuncExprResolver resolver(ctx);
        ObProxyExpr *expr;

        if (OB_FAIL(parser.parse(default_value_expr, result))) {
          LOG_INFO("parse default value expr failed", K(ret));
        } else if (OB_FAIL(resolver.resolve(result.param_node_, expr))) {
          LOG_INFO("proxy expr resolve failed", K(ret));
        } else {
          ObSEArray<ObObj, 4> result_array;
          ObProxyExprCalcItem calc_item(const_cast<SqlFieldResult *>(sql_field_result));
          ObProxyExprCtx expr_ctx(0, TESTLOAD_NON, false, &allocator_, client_session_info);
          expr_ctx.is_oracle_mode = is_oracle_mode;
          if (OB_FAIL(expr->calc(expr_ctx, calc_item, result_array))) {
            LOG_WARN("calc expr result failed", K(ret));
          } else if (OB_FAIL(result_array.at(0, *target_obj))) {
            LOG_WARN("get expr calc result fail", K(ret));
          }
        }
      }
    } else {
      // mysql mode return the default value with resolved obj in column's type
      // do nothing 
    }
  }
  if (OB_SUCC(ret) && ObStringTC == target_obj->get_type_class()) {
    LOG_DEBUG("parse and resolve default value succ", K(*target_obj), K(ret));
    target_obj->set_collation_type(static_cast<common::ObCollationType>(client_session_info->get_collation_connection()));
  }
  return ret;
}

int ObExprResolver::convert_token_node_to_param_node(ObProxyTokenNode *token,
                                                     ObProxyParamNode *&param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(token)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret));
  } else {
    void *tmp_buf = NULL;
    if (OB_ISNULL(tmp_buf = allocator_.alloc(sizeof(ObProxyParamNode)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("fail to alloc new param node", K(ret));
    } else {
      param = new(tmp_buf) ObProxyParamNode();
      param->next_ = NULL;
      if (TOKEN_INT_VAL == token->type_) {
        param->int_value_ = token->int_value_;
        param->type_ = PARAM_INT_VAL;
      } else if (TOKEN_STR_VAL == token->type_) {
        param->str_value_ = token->str_value_;
        param->type_ = PARAM_STR_VAL;
      } else if (TOKEN_FUNC == token->type_) {
        if (OB_FAIL(recursive_convert_func_token(token, param))) {
          LOG_WARN("convert func token node to param node failed", K(ret));
        }
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("unexpected token node type, please check", K(ret), K(param->type_));
      }
    }
  }
  return ret;
}

int ObExprResolver::recursive_convert_func_token(ObProxyTokenNode *token,
                                                 ObProxyParamNode *param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(token) || OB_ISNULL(param) || token->type_ != TOKEN_FUNC) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret));
  }

  ObString func_name(token->str_value_.str_len_, token->str_value_.str_);
  ObProxyExprType func_type = get_expr_token_func_type(&func_name);
  if (OB_SUCC(ret) && func_type == OB_PROXY_EXPR_TYPE_NONE) {
    ret = OB_ERR_FUNCTION_UNKNOWN;
    LOG_WARN("unsupported func type", K(ret), K(func_name), K(func_type));
  }

  if (OB_SUCC(ret)) {
    void *tmp_buf = NULL;
    param->type_ = PARAM_FUNC;
    if (OB_ISNULL(tmp_buf = allocator_.alloc(sizeof(ObFuncExprNode)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("fail to alloc new func expr node", K(ret));
    } else {
      param->func_expr_node_ = new(tmp_buf) ObFuncExprNode();
      param->func_expr_node_->func_type_ = func_type;
      param->func_expr_node_->child_ = NULL;

      if (OB_ISNULL(token->child_)) {
        // do nothing
      } else if (OB_ISNULL(tmp_buf = allocator_.alloc(sizeof(ObProxyParamNodeList)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("fail to alloc new token list", K(ret));
      } else {
        param->func_expr_node_->child_ = new(tmp_buf) ObProxyParamNodeList();
        ObProxyParamNode head;
        ObProxyParamNode *param_cur = &head;
        ObProxyTokenNode *token_child = token->child_->head_;
        int64_t child_num = 0;
        for (; OB_SUCC(ret) && token_child != NULL; token_child = token_child->next_, param_cur = param_cur->next_) {
          if (OB_FAIL(convert_token_node_to_param_node(token_child, param_cur->next_))) {
            LOG_WARN("recursive convert func token failed", K(ret));
          } else {
            child_num++;
          }
        }
        if (OB_SUCC(ret)) {
          param->func_expr_node_->child_->tail_ = param_cur;
          param->func_expr_node_->child_->head_ = head.next_;
          param->func_expr_node_->child_->child_num_ = child_num;
        }
      }
    }
  }
  return ret;
}

int ObExprResolver::calc_generated_key_value(ObObj &obj, const ObProxyPartKey &part_key, const bool is_oracle_mode)
{
  int ret = OB_SUCCESS;
  if (OB_LIKELY(OB_PROXY_EXPR_TYPE_FUNC_SUBSTR == part_key.func_type_)) {
    //  we only support substr now
    int64_t start_pos = INT64_MAX;
    int64_t sub_len = INT64_MAX;
    if (NULL != part_key.params_[1] && PARAM_INT_VAL == part_key.params_[1]->type_) {
      start_pos = part_key.params_[1]->int_value_;
    }
    if (NULL != part_key.params_[2] && PARAM_INT_VAL == part_key.params_[2]->type_) {
      sub_len = part_key.params_[2]->int_value_;
    }
    ObString src_val;
    if (obj.is_varchar()) {
      if (OB_FAIL(obj.get_varchar(src_val))) {
        LOG_WARN("fail to get varchar value", K(obj), K(ret));
      } else {
        if (start_pos < 0) {
          start_pos = src_val.length() + start_pos + 1;
        }
        if (0 == start_pos && is_oracle_mode) {
          start_pos = 1;
        }
        if (INT64_MAX == sub_len) {
          sub_len = src_val.length() - start_pos + 1;
        }
        if (start_pos > 0 && start_pos <= src_val.length()
            && sub_len > 0 && sub_len <= src_val.length()) {
            obj.set_varchar(src_val.ptr() + start_pos - 1, static_cast<int32_t>(sub_len));
        }
      }
    }
  } else {
    ret = OB_ERR_FUNCTION_UNKNOWN;
    LOG_WARN("unknown generate function type", K(part_key.func_type_), K(ret));
  }
  return ret;
}

int ObExprResolver::get_obj_with_param(ObObj &target_obj,
                                       ObProxyMysqlRequest *client_request,
                                       ObClientSessionInfo *client_info,
                                       ObProxyPartInfo *part_info,
                                       ObPsIdEntry *ps_id_entry,
                                       const int64_t param_index)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(client_request) || OB_ISNULL(client_info) || OB_UNLIKELY(param_index < 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(client_request), K(param_index), K(ret));
  } else {
    int64_t execute_param_index = param_index;
    bool need_use_execute_param = false;
    // here parse result means the original parse result for this ps sql or call sql
    ObSqlParseResult &parse_result = client_request->get_parse_result();
    ObProxyCallInfo &call_info = parse_result.call_info_;
    if (parse_result.is_call_stmt() || parse_result.is_text_ps_call_stmt()) {
      if (OB_UNLIKELY(!call_info.is_valid()) || OB_UNLIKELY(param_index >= call_info.param_count_)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("invalid placeholder idx", K(param_index), K(call_info), K(ret));
      } else {
        const ObProxyCallParam* call_param = call_info.params_.at(param_index);
        if (CALL_TOKEN_INT_VAL == call_param->type_) {
          int64_t int_val = 0;
          if (OB_FAIL(get_int_value(call_param->str_value_.config_string_, int_val))) {
            LOG_WARN("fail to get int value", K(call_param->str_value_.config_string_), K(ret));
          } else {
            target_obj.set_int(int_val);
          }
        } else if (CALL_TOKEN_STR_VAL == call_param->type_) {
          target_obj.set_varchar(call_param->str_value_.config_string_);
          target_obj.set_collation_type(ObCharset::get_default_collation(ObCharset::get_default_charset()));
        } else if (CALL_TOKEN_PLACE_HOLDER == call_param->type_) {
          need_use_execute_param = true;
          if (OB_FAIL(get_int_value(call_param->str_value_.config_string_, execute_param_index))) {
            LOG_WARN("fail to get int value", K(call_param->str_value_.config_string_), K(ret));
          }
        }
      }
    } else {
      need_use_execute_param = true;
    }
    if (OB_SUCC(ret)
        && need_use_execute_param
        && OB_MYSQL_COM_STMT_EXECUTE == client_request->get_packet_meta().cmd_) {
      // for com_stmt_prepare, we have no execute_params, so no need continue, just return
      LOG_DEBUG("will cal obj with value from execute param", K(execute_param_index));
      if (OB_ISNULL(ps_id_entry)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("client ps id entry is null", K(ret), KPC(ps_id_entry));
      } else if (OB_UNLIKELY(execute_param_index >= ps_id_entry->get_param_count())
                 || execute_param_index < 0) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("invalid placeholder idx", K(execute_param_index), KPC(ps_id_entry), K(ret));
      } else if (OB_FAIL(ObMysqlRequestAnalyzer::analyze_execute_param(ps_id_entry->get_param_count(),
                         ps_id_entry->get_ps_sql_meta().get_param_types(), *client_request, execute_param_index, target_obj))) {
        LOG_WARN("fail to analyze execute param", K(ret));
      }
    }
    if (OB_SUCC(ret) && need_use_execute_param && OB_MYSQL_COM_STMT_PREPARE == client_request->get_packet_meta().cmd_) {
      ret = OB_INVALID_ARGUMENT;
      LOG_DEBUG("prepare sql with only placeholder, will return fail", K(ret));
    }

    if (OB_SUCC(ret)
        && need_use_execute_param
        && client_request->get_parse_result().is_text_ps_execute_stmt()) {
      LOG_DEBUG("will cal obj with value from ps execute param", K(execute_param_index));
      ObSqlParseResult &parse_result = client_request->get_parse_result();
      ObProxyTextPsInfo execute_info = parse_result.text_ps_info_;
      if (execute_param_index >= execute_info.param_count_) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("param index is large than param count", K(execute_param_index),
            K(execute_info.param_count_), K(ret));
      } else {
        ObProxyTextPsParam* param = execute_info.params_.at(execute_param_index);
        ObString user_variable_name = param->str_value_.config_string_;
        if (OB_FAIL(static_cast<const ObClientSessionInfo&>(*client_info).get_user_variable_value(user_variable_name, target_obj))) {
          LOG_WARN("get user variable failed", K(ret), K(user_variable_name));
        } else {
          ObString user_var;
          int tmp_ret = OB_SUCCESS;
          if (target_obj.is_varchar()) {
            if (OB_SUCCESS != (tmp_ret = target_obj.get_varchar(user_var))) {
              LOG_WARN("get varchar failed", K(tmp_ret));
            } else {
              char* ptr = user_var.ptr();
              int32_t len = user_var.length();
              // user var has store ' into value
              if ((user_var[0] == 0x27 && user_var[len-1] == 0x27) ||
                (user_var[0] == 0x22 && user_var[len-1] == 0x22)) {
                target_obj.set_varchar(ptr + 1, len - 2);
              }
            }
          }
        }
      }
    }

    if (OB_SUCC(ret)
        && need_use_execute_param
        && OB_MYSQL_COM_STMT_PREPARE_EXECUTE == client_request->get_packet_meta().cmd_) {
      LOG_DEBUG("will cal obj with value from execute param", K(execute_param_index));
      if (OB_UNLIKELY(execute_param_index < 0)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("invalid placeholder idx", K(execute_param_index), K(ret));
      } else if (OB_FAIL(ObMysqlRequestAnalyzer::analyze_prepare_execute_param(*client_request, execute_param_index, target_obj))) {
        LOG_WARN("fail to analyze execute param", K(ret));
      }
    }

    if (OB_SUCC(ret)
        && need_use_execute_param
        && OB_MYSQL_COM_STMT_SEND_LONG_DATA == client_request->get_packet_meta().cmd_) {
      LOG_DEBUG("will calc obj with execute param for send long data");
      if (OB_FAIL(ObMysqlRequestAnalyzer::analyze_send_long_data_param(*client_request, execute_param_index,
                                                                       part_info, ps_id_entry, target_obj))) {
        LOG_DEBUG("fail to analyze send long data param", K(ret));
      }
    }
  }
  return ret;
}

ObProxyExprType ObExprResolver::get_expr_token_func_type(ObString *func)
{
  ObProxyExprType type = OB_PROXY_EXPR_TYPE_NONE;
  if (func == NULL) {
    type = OB_PROXY_EXPR_TYPE_NONE;
  } else if (0 == func->case_compare("HASH")) {
    type = OB_PROXY_EXPR_TYPE_FUNC_HASH;
  } else if (0 == func->case_compare("SUBSTR")) {
    type = OB_PROXY_EXPR_TYPE_FUNC_SUBSTR;
  } else if (0 == func->case_compare("CONCAT")) {
    type = OB_PROXY_EXPR_TYPE_FUNC_CONCAT;
  } else if (0 == func->case_compare("TOINT")) {
    type = OB_PROXY_EXPR_TYPE_FUNC_TOINT;
  } else if (0 == func->case_compare("DIV")) {
    type = OB_PROXY_EXPR_TYPE_FUNC_DIV;
  } else if (0 == func->case_compare("ADD")) {
    type = OB_PROXY_EXPR_TYPE_FUNC_ADD;
  } else if (0 == func->case_compare("SUB")) {
    type = OB_PROXY_EXPR_TYPE_FUNC_SUB;
  } else if (0 == func->case_compare("MUL")) {
    type = OB_PROXY_EXPR_TYPE_FUNC_MUL;
  } else if (0 == func->case_compare("SUM")) {
    type = OB_PROXY_EXPR_TYPE_FUNC_SUM;
  } else if (0 == func->case_compare("CONUT")) {
    type = OB_PROXY_EXPR_TYPE_FUNC_COUNT;
  } else if (0 == func->case_compare("MAX")) {
    type = OB_PROXY_EXPR_TYPE_FUNC_MAX;
  } else if (0 == func->case_compare("MIN")) {
    type = OB_PROXY_EXPR_TYPE_FUNC_MIN;
  } else if (0 == func->case_compare("AVG")) {
    type = OB_PROXY_EXPR_TYPE_FUNC_AVG;
  } else if (0 == func->case_compare("GROUP")) {
    type = OB_PROXY_EXPR_TYPE_FUNC_GROUP;
  } else if (0 == func->case_compare("ORDER")) {
    type = OB_PROXY_EXPR_TYPE_FUNC_ORDER;
  } else if (0 == func->case_compare("TESTLOAD")) {
    type = OB_PROXY_EXPR_TYPE_FUNC_TESTLOAD;
  } else if (0 == func->case_compare("SPLIT")) {
    type = OB_PROXY_EXPR_TYPE_FUNC_SPLIT;
  } else if (0 == func->case_compare("TO_DATE")) {
    type = OB_PROXY_EXPR_TYPE_FUNC_TO_DATE;
  } else if (0 == func->case_compare("TO_TIMESTAMP")) {
    type = OB_PROXY_EXPR_TYPE_FUNC_TO_TIMESTAMP;
  } else if (0 == func->case_compare("NVL")) {
    type = OB_PROXY_EXPR_TYPE_FUNC_NVL;
  } else if (0 == func->case_compare("TO_CHAR")) {
    type = OB_PROXY_EXPR_TYPE_FUNC_TO_CHAR;
  } else if (0 == func->case_compare("SYSDATE")) {
    type = OB_PROXY_EXPR_TYPE_FUNC_SYSDATE;
  } else if (0 == func->case_compare("MOD")) {
    type = OB_PROXY_EXPR_TYPE_FUNC_MOD;
  } else {
    type = OB_PROXY_EXPR_TYPE_NONE;
  }
  return type;
}

} // end of opsql
} // end of obproxy
} // end of oceanbase
