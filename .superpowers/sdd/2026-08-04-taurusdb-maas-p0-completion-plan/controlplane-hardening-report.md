# Control-plane hardening report

## Scope

The protected table is `mysql.taurusdb_ai_model_config`. The prior
`sql_authorization` gate only required `AI_ADMIN`, which meant a normal SQL
client holding that dynamic grant plus table DML/DDL privileges could bypass
`dbms_ai` and publish arbitrary rows.

The hardened gate now rejects direct SQL `INSERT`, `UPDATE`, `DELETE`,
`ALTER`, `DROP`, and index changes on that table regardless of client
privileges. It is evaluated in both `check_table_access()` and
`check_grant()` because system-table paths can reach the latter directly.
`GRANT`/`REVOKE` are not writes to the protected table and remain possible;
they cannot bypass the statement-time table gate.

The only statement-path exceptions are:

- replica appliers (`slave_thread`), replaying a primary-authorized row event;
- data-dictionary/bootstrap initialization threads;
- server upgrade threads.

`dbms_ai` itself continues using `System_table_access`, which opens the table
outside client SQL authorization and still requires `AI_ADMIN` in its native
procedure command. This preserves package writes and the existing
`ai_maas_model_admin_rpl` replication coverage.

## Credential publishing

In Release builds, `register_model()` and `update_model()` now construct only
an internal `Ai_resolved_model` reference and call the existing
`Ai_credential_resolver::ReadSecret()` keyring-reader path before opening the
control table. The fetched value is held only in `Secure_string`, whose
destructor cleans it. An unreadable or empty reference fails before any row
write and the package diagnostic does not include Secret bytes. A keyring
implementation can independently log a missing reference identifier; reference
identifier confidentiality is therefore an integration-policy decision, while
Secret bytes are never passed to package diagnostics, audit records or SQL
results.

Debug builds deliberately do not make offline MTR depend on a keyring. Their
two exact `mtr/fixture-*` profiles are compiled only without `NDEBUG`, are
registered through `dbms_ai`, and return local deterministic responses. They
are physically removed by the Debug-only fixture cleanup branch so separate
MTR cases can reuse the exact fixture name. Normal profiles, including all
Release profiles, retain tombstone-on-delete behavior.

## Test-first evidence

1. Added `ai_maas_model_admin` assertions for a client holding `AI_ADMIN` and
   every relevant table DML/DDL privilege. The first harmless `INSERT ...
   SELECT ... WHERE FALSE` initially succeeded, proving the old bypass.
2. Added governed offline fixture registration in `ai_maas_embedding`. It
   initially failed because `dbms_ai` did not recognize the exact Debug
   fixture profile.
3. Implemented the SQL authorization gate and the Debug-only fixture package
   path, then migrated all RDS MaaS fixture setup/cleanup from direct control
   table DML to `dbms_ai` calls.

## Verification

Debug build:

```text
cmake --build build-debug --target mysqld -j 8
cd build-debug/mysql-test
./mtr --suite=rds --record ai_maas_embedding ai_maas_analysis \
  ai_maas_contract ai_maas_governance ai_maas_rag ai_maas_model_admin \
  ai_maas_model_admin_rpl
```

Result: all seven RDS tests and `shutdown_report` passed. The model-admin
case proves direct DML, ALTER, and DROP INDEX are rejected while the same
`AI_ADMIN` client can register, update and delete through `dbms_ai`. The
replication test passed after the gate change.

Release build:

```text
cmake --build build-release --target mysqld -j 8
```

Result: succeeded. `ai_maas_model_admin_release` is the Release-only MTR
contract: it skips on Debug, creates an isolated component keyring with fake
data, publishes one readable Profile, removes that reference, then verifies
the failed `update_model()` leaves its active version at 1. It also verifies
that an unknown reference makes `register_model()` fail without publishing an
active Profile. After building the complete Release runtime tools,
`./mtr --suite=rds ai_maas_model_admin_release` passed.

## Remaining target-environment validation

The repository cannot prove a customer keyring/CSMS reference is present or
readable. Release acceptance must register and update a real, non-empty
`SECRET_REF` in the TaurusDB target environment, then rotate or remove it and
confirm later invocation fails before MaaS egress. That test must not print
the secret or store it in an MTR result.
