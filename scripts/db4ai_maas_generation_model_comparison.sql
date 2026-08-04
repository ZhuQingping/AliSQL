-- Huawei MaaS text-generation comparison: two fixed use cases across six
-- configured model profiles.
--
-- Run from the mysql client after selecting a database:
--   mysql> source /absolute/path/to/db4ai_maas_generation_model_comparison.sql
--
-- This script makes 12 MaaS requests. It uses only already configured model
-- profiles and never stores credential material.

DROP TEMPORARY TABLE IF EXISTS db4ai_generation_model_results;
DROP TEMPORARY TABLE IF EXISTS db4ai_generation_model_cases;
DROP TEMPORARY TABLE IF EXISTS db4ai_generation_model_models;
DROP PROCEDURE IF EXISTS db4ai_run_generation_model_comparison;

CREATE TEMPORARY TABLE db4ai_generation_model_cases (
  use_case VARCHAR(32) NOT NULL,
  task TEXT NOT NULL,
  input_json JSON NOT NULL,
  PRIMARY KEY (use_case)
);

CREATE TEMPORARY TABLE db4ai_generation_model_results (
  use_case VARCHAR(32) NOT NULL,
  model_name VARCHAR(255) NOT NULL,
  elapsed_ms BIGINT UNSIGNED NOT NULL,
  status ENUM('PASS', 'FAIL') NOT NULL,
  analysis_result LONGTEXT NULL,
  error_message TEXT NULL,
  PRIMARY KEY (use_case, model_name)
);

CREATE TEMPORARY TABLE db4ai_generation_model_models (
  model_name VARCHAR(255) NOT NULL,
  PRIMARY KEY (model_name)
);

INSERT INTO db4ai_generation_model_cases (use_case, task, input_json) VALUES
  ('slow_sql_diagnose',
   '仅根据给出的 orders 查询证据诊断慢 SQL：说明范围扫描的原因，并给出可验证的索引或查询改写建议。',
   JSON_OBJECT(
     'sql', 'SELECT id, customer_id, total_amount FROM orders WHERE created_at >= ''2026-07-01'' AND created_at < ''2026-08-01'' AND status = ''PAID'' ORDER BY created_at DESC LIMIT 100',
     'explain', JSON_OBJECT('access_type', 'range', 'key', 'ix_orders_created_at',
                            'rows_examined', 180000, 'filtered_percent', 10.0,
                            'extra', 'Using index condition; Using where; Using filesort'),
     'table_statistics', JSON_OBJECT('orders_rows', 1800000,
                                     'paid_orders_in_range', 18000,
                                     'observed_elapsed_ms', 4800))),
  ('order_business_analysis',
   '仅根据给出的月度订单和营收证据进行业务分析：总结环比变化、指出值得跟进的信号，并给出下一步建议。',
   JSON_OBJECT(
     'monthly_orders', JSON_ARRAY(
       JSON_OBJECT('month', '2026-06', 'orders', 12000, 'revenue', 3600000),
       JSON_OBJECT('month', '2026-07', 'orders', 10800, 'revenue', 3024000)),
     'month_over_month', JSON_OBJECT('orders_change_percent', -10.0,
                                     'revenue_change_percent', -16.0,
                                     'average_order_value_change_percent', -6.7)));

INSERT INTO db4ai_generation_model_models (model_name) VALUES
  ('huawei/glm-5.2'),
  ('huawei/kimi-k2.6'),
  ('huawei/deepseek-v4-pro'),
  ('huawei/deepseek-v4-flash'),
  ('huawei/openpangu-2.0-pro'),
  ('huawei/openpangu-2.0-flash');

DELIMITER //
CREATE PROCEDURE db4ai_run_generation_model_comparison()
BEGIN
  DECLARE v_done BOOL DEFAULT FALSE;
  DECLARE v_call_failed BOOL DEFAULT FALSE;
  DECLARE v_use_case VARCHAR(32);
  DECLARE v_task TEXT;
  DECLARE v_input_json LONGTEXT;
  DECLARE v_model_name VARCHAR(255);
  DECLARE v_started DATETIME(6);
  DECLARE v_elapsed_ms BIGINT UNSIGNED;

  DECLARE db4ai_case_model_cursor CURSOR FOR
    SELECT c.use_case, c.task, c.input_json, m.model_name
      FROM db4ai_generation_model_cases AS c
      CROSS JOIN db4ai_generation_model_models AS m
     ORDER BY c.use_case, m.model_name;
  DECLARE CONTINUE HANDLER FOR NOT FOUND SET v_done = TRUE;
  DECLARE CONTINUE HANDLER FOR SQLEXCEPTION SET v_call_failed = TRUE;

  OPEN db4ai_case_model_cursor;
  db4ai_case_model_loop: LOOP
    FETCH db4ai_case_model_cursor
      INTO v_use_case, v_task, v_input_json, v_model_name;
    IF v_done THEN
      LEAVE db4ai_case_model_loop;
    END IF;

    SET v_call_failed = FALSE;
    SET @db4ai_result = NULL;
    SET v_started = NOW(6);
    SET @db4ai_generation_model_sql = CONCAT(
      'SET @db4ai_result = AI_ANALYZE(',
      QUOTE(v_model_name), ', ',
      QUOTE(CONCAT(v_task, '\n\n证据(JSON)：\n', v_input_json)), ', ',
      'JSON_OBJECT(''max_output_tokens'', 512',
      ', ''timeout_ms'', 60000))');
    PREPARE db4ai_generation_model_statement
      FROM @db4ai_generation_model_sql;
    EXECUTE db4ai_generation_model_statement;
    DEALLOCATE PREPARE db4ai_generation_model_statement;
    SET v_elapsed_ms =
      TIMESTAMPDIFF(MICROSECOND, v_started, NOW(6)) DIV 1000;

    IF v_call_failed OR @db4ai_result IS NULL THEN
      INSERT INTO db4ai_generation_model_results
        (use_case, model_name, elapsed_ms, status, analysis_result, error_message)
      VALUES
        (v_use_case, v_model_name, v_elapsed_ms, 'FAIL', NULL,
         'AI_ANALYZE invocation failed');
    ELSE
      INSERT INTO db4ai_generation_model_results
        (use_case, model_name, elapsed_ms, status, analysis_result, error_message)
      VALUES
        (v_use_case, v_model_name, v_elapsed_ms, 'PASS', @db4ai_result, NULL);
    END IF;
  END LOOP;
  CLOSE db4ai_case_model_cursor;
END//
DELIMITER ;

CALL db4ai_run_generation_model_comparison();

SELECT use_case, model_name, elapsed_ms, status, analysis_result, error_message
  FROM db4ai_generation_model_results
 ORDER BY use_case, model_name;
SELECT status, COUNT(*) AS result_count
  FROM db4ai_generation_model_results
 GROUP BY status
 ORDER BY status;

DROP PROCEDURE db4ai_run_generation_model_comparison;
DROP TEMPORARY TABLE db4ai_generation_model_models;
DROP TEMPORARY TABLE db4ai_generation_model_cases;
DROP TEMPORARY TABLE db4ai_generation_model_results;
