# librdkafka verifiable clients for Apache Kafka system tests

Three C binaries that implement Apache Kafka's "verifiable client" contract
so the Ducktape-based Python system tests in `apache/kafka` can exercise
librdkafka the same way they exercise the Java client:

- `verifiable_producer/` — produces messages with optional throttling
- `verifiable_consumer/` — subscribing consumer (classic + KIP-848 protocols)
- `verifiable_share_consumer/` — KIP-932 share consumer

Each binary accepts a standardized CLI and emits newline-delimited JSON
events on stdout. The Python harness in
`apache/kafka/tests/kafkatest/services/verifiable_*.py` parses those events.

## Build

Requires librdkafka's own build prerequisites. From the repo root:

```
./configure
make libs
make -C tests/kafkatest
```

Binaries land at:

- `tests/kafkatest/verifiable_producer/verifiable_producer`
- `tests/kafkatest/verifiable_consumer/verifiable_consumer`
- `tests/kafkatest/verifiable_share_consumer/verifiable_share_consumer`

They statically link `../../src/librdkafka.a`, so each is a single
self-contained file that can be copied to a test node.

## Running under Ducktape

The harness discovers these binaries via `globals.json` in this
directory. Point Ducktape at it with `--globals`:

```
ducktape --globals tests/kafkatest/globals.json <test-file>
```

The harness resolves each `Verifiable*` class to
`VerifiableClientApp`, which SSHes into each worker and runs:

1. `deploy.sh` — once per node, installs apt packages and builds.
2. `exec_cmd` + the harness-supplied `--topic`, `--bootstrap-server`,
   `--max-messages`, etc.

The paths in `globals.json` assume the librdkafka repo is mounted at
`/librdkafka` on the worker. `deploy.sh` honors `LIBRDKAFKA_DIR` to
override that. Pass `DEPLOY_SKIP_APT=1` when running on a pre-provisioned
image where the build deps (`build-essential`, `libssl-dev`,
`libsasl2-dev`, `libz-dev`, `liblz4-dev`, `libzstd-dev`,
`libcurl4-openssl-dev`, `pkg-config`) are already installed.

`deploy.sh` is idempotent via a sentinel file (`.deployed`) so
re-invocations on the same node skip the rebuild.

## Status

- Phase 1 (verifiable_producer) — complete
- Phase 2 (verifiable_consumer) — complete, including the KIP-848
  empty-assignment workaround via `stats_cb`
- Phase 3 (verifiable_share_consumer) — complete for
  `--acknowledgement-mode=sync`, including `--offset-reset-strategy
  earliest|latest` (sets `share.auto.offset.reset` on the group via
  `IncrementalAlterConfigs` before subscribe, matching Java's
  `VerifiableShareConsumer`). See "Known gaps" below.
- Phase 4 (deploy + globals) — complete
- Phase 5 (Semaphore CI) — pending

### Known gaps

- **KIP-848 empty-assignment rebalance callback.** When
  `group.protocol=consumer` and a consumer joins with zero assigned
  partitions, librdkafka skips the rebalance callback
  (`src/rdkafka_cgrp.c:rd_kafka_cgrp_consumer_is_new_assignment_different`
  short-circuits because "current == target" for two empty lists). The
  Java client always fires the callback, and the Ducktape harness
  expects the event. `verifiable_consumer` works around this in its
  `stats_cb`: when it sees `cgrp.state=up`, `stateage<2000ms`, and
  `assignment_size=0`, it synthesizes a `partitions_assigned` event
  with an empty list. Scoped to consumer protocol only; classic
  protocol is unaffected. TODO: remove the workaround once librdkafka
  fixes the underlying bug.

- **Share consumer async/auto `offsets_acknowledged`.**
  `--acknowledgement-mode=async` and `--acknowledgement-mode=auto`
  (implicit) do NOT emit `offsets_acknowledged` events. Java's
  `AcknowledgementCommitCallback` has no equivalent in librdkafka yet,
  so there is no way to learn per-partition commit results for these
  modes. Sync mode has full parity (commit_sync returns the per-partition
  result list directly). TODO: revisit when librdkafka adds the callback.

## Contract sources

- Python harness: `apache/kafka/tests/kafkatest/services/verifiable_*.py`
- Java reference: `apache/kafka/tools/src/main/java/org/apache/kafka/tools/Verifiable*.java`
- Go POC (consumer + producer only): `confluent-kafka-go/kafkatest/`
