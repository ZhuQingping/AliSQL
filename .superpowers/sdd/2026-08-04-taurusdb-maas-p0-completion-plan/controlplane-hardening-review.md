# Control-plane hardening review

Findings:

  Critical:
  - None.

  Important:
  - None.

  Minor:
  - None.

Open questions:

  - None.  The `NDEBUG` guard is appropriate for this tree: the configured
    RelWithDebInfo release build supplies `-DNDEBUG`, and the fixture predicate
    is unconditionally false in that build.

Verification:

  - Reviewed the current uncommitted diff in `sql/auth/sql_authorization.cc`,
    `sql/ai/ai_model_admin.cc`, and the corresponding RDS MTR contracts.
  - `git diff --check` completed without whitespace errors.
  - The authorization gate is applied in both `check_table_access()` and
    `check_grant()` before normal privilege resolution.  It denies the six
    requested write privileges independently of `AI_ADMIN` and table grants;
    `GRANT`/`REVOKE` are allowed only to manage those grants, while a direct
    DML/DDL statement remains gated.  Native `dbms_ai` writes use
    `System_table_access`; bootstrap/upgrade and replication-applier THDs are
    the server-owned SQL exceptions.
  - `Register()` and `Update()` call `ValidRequest()` before opening the
    table.  In NDEBUG builds that path calls the existing keyring reader and
    requires a nonempty `Secure_string`; its failure surfaces only the generic
    request diagnostic.  Debug bypasses that probe, while the two offline
    fixture identities cannot satisfy the fixture predicate under NDEBUG.  The
    latest formatting-only expansion of `Delete()` was also inspected and has
    no semantic effect on these controls.
  - The revised `rds.ai_maas_model_admin_release` contract now starts an
    isolated component-keyring-file instance with a synthetic nonempty secret,
    successfully publishes version 1, removes that reference and restarts,
    then proves an unreadable `update_model()` leaves the active profile and
    version unchanged.  It also proves an unreadable `register_model()` leaves
    no model discoverable through `AI_MODEL_INFO()`.  The fresh Release log is
    timestamped 2026-08-04 12:52:03 and contains only the expected missing
    reference diagnostics, not the synthetic secret value.
  - Fresh Debug MTR evidence covers the seven RDS cases through
    `rds.ai_maas_model_admin_rpl`; the server log is timestamped
    2026-08-04 12:51:51.  The supplied verification reports both Debug and
    Release invocations passed.  `git diff --check` remains clean.

Recommendation:

  ACCEPT
