#!/bin/bash
#
# Run apache/kafka Ducktape system tests against our librdkafka verifiable
# clients. Invoked from .semaphore/semaphore-integration.yml.
#
# Assumptions:
#   - Running on an Ubuntu Semaphore agent (bash, apt, docker, python3).
#   - The librdkafka checkout is at $SEMAPHORE_GIT_DIR (Semaphore default)
#     or $LIBRDKAFKA_DIR if overridden.
#   - Docker is available and the agent user is in the docker group.
#   - 16 GB RAM (s1-prod-ubuntu24-04-amd64-2). Tuned around that budget.
#
# Exits non-zero if any test fails. Dumps results to $SEMAPHORE_GIT_DIR/
# artifacts/ducktape-results so `artifact push` can upload them.

set -euo pipefail

# Resolve LIBRDKAFKA_DIR to an absolute path. On Semaphore the script
# runs from inside the checkout (cwd == librdkafka repo), so `pwd` is
# the right answer. If LIBRDKAFKA_DIR was set explicitly, honor it.
# ducker-ak's docker -v flag needs an absolute path or Docker rejects
# the mount.
if [[ -n "${LIBRDKAFKA_DIR:-}" ]]; then
    LIBRDKAFKA_DIR="$(cd "${LIBRDKAFKA_DIR}" && pwd)"
else
    LIBRDKAFKA_DIR="$(pwd)"
fi
KAFKA_REF="${KAFKA_REF:-4.2.0}"
KAFKA_CHECKOUT="${KAFKA_CHECKOUT:-$(dirname "${LIBRDKAFKA_DIR}")/apache-kafka}"
NUM_NODES="${NUM_NODES:-11}"
DUCKER_MEM="${DUCKER_MEM:-1300m}"

# Gradle (invoked below to build apache/kafka systemTestLibs) needs
# JDK 17+ for Kafka 4.2.0. The Semaphore agent ships multiple JDKs;
# if $JAVA_HOME isn't already 17+ or we can't tell, point to the 17
# install if it exists.
if [[ -z "${JAVA_HOME:-}" ]] || ! "${JAVA_HOME}/bin/java" -version 2>&1 | \
      grep -qE 'version "(1[7-9]|[2-9][0-9])'; then
    for cand in \
        /usr/lib/jvm/java-17-openjdk-amd64 \
        /usr/lib/jvm/temurin-17-jdk-amd64 \
        /opt/java/openjdk \
        $(ls -d /usr/lib/jvm/*17* 2>/dev/null | head -1); do
        if [[ -x "${cand}/bin/java" ]]; then
            export JAVA_HOME="${cand}"
            export PATH="${JAVA_HOME}/bin:${PATH}"
            echo "=== using JAVA_HOME=${JAVA_HOME} ==="
            break
        fi
    done
fi

# Tests to run. Space-separated test spec paths (relative to the
# apache/kafka repo). Can be overridden via env var.
#
# Demo set: one method per suite, all proven green locally. Combined
# with --sample 1 below we get 5 actual test runs per CI invocation.
TESTS_TO_RUN="${TESTS_TO_RUN:-\
    tests/kafkatest/tests/client/pluggable_test.py::PluggableConsumerTest.test_start_stop \
    tests/kafkatest/tests/client/compression_test.py::CompressionTest.test_compressed_topic \
    tests/kafkatest/tests/client/consumer_test.py::OffsetValidationTest.test_fencing_static_consumer \
    tests/kafkatest/tests/client/share_consumer_test.py::ShareConsumerTest.test_share_single_topic_partition \
    tests/kafkatest/tests/client/truncation_test.py::TruncationTest.test_offset_truncate}"

echo "=== librdkafka system-test runner ==="
echo "LIBRDKAFKA_DIR=${LIBRDKAFKA_DIR}"
echo "KAFKA_CHECKOUT=${KAFKA_CHECKOUT}"
echo "KAFKA_REF=${KAFKA_REF}"
echo "NUM_NODES=${NUM_NODES}"
echo "DUCKER_MEM=${DUCKER_MEM}"
echo "TESTS_TO_RUN=${TESTS_TO_RUN}"

# --- apache/kafka checkout ------------------------------------------------

if [[ ! -d "${KAFKA_CHECKOUT}/.git" ]]; then
    echo "=== cloning apache/kafka@${KAFKA_REF} ==="
    git clone --depth 1 --branch "${KAFKA_REF}" \
        https://github.com/apache/kafka.git "${KAFKA_CHECKOUT}"
else
    echo "=== apache/kafka already present at ${KAFKA_CHECKOUT}, reusing ==="
    (cd "${KAFKA_CHECKOUT}" && git fetch --depth 1 origin "${KAFKA_REF}" && \
        git checkout -q FETCH_HEAD) || true
fi

# --- ducker-ak patches ----------------------------------------------------

# 1. Mount librdkafka into every worker container.
# 2. Shrink per-container memory from the 2000m default.
# Both edits are idempotent: grep-then-sed so re-runs on a cached checkout
# don't stack multiple patches.
DUCKER="${KAFKA_CHECKOUT}/tests/docker/ducker-ak"
if ! grep -q "LIBRDKAFKA_DIR" "${DUCKER}"; then
    echo "=== patching ducker-ak to honor LIBRDKAFKA_DIR ==="
    # Insert librdkafka mount flag handling into docker_run(). We insert
    # the conditional mount block just before the `must_do ... run` line
    # inside docker_run(), and append the flag to that command.
    python3 - <<PYEOF
import re, sys
p = "${DUCKER}"
s = open(p).read()
old = '        -v "\${kafka_dir}:/opt/kafka-dev" --name "\${node}" -- "\${image_name}"'
new = '''        -v "\${kafka_dir}:/opt/kafka-dev" \${librdkafka_mount} --name "\${node}" -- "\${image_name}"'''
# Add the variable definition above the must_do run.
hook = '    must_do -v \${container_runtime} run --init --privileged \\\\'
addition = '''    local librdkafka_mount=""
    if [[ -n "\${LIBRDKAFKA_DIR}" ]]; then
        librdkafka_mount="-v \${LIBRDKAFKA_DIR}:/librdkafka"
    fi
'''
if old not in s:
    sys.exit("docker_run volume line not found; ducker-ak upstream changed?")
s = s.replace(old, new)
s = s.replace(hook, addition + hook)
open(p, "w").write(s)
PYEOF
fi

# Memory override: reduce from 2000m to DUCKER_MEM.
sed -i.bak -E "s/^docker_run_memory_limit=\"[0-9]+m\"/docker_run_memory_limit=\"${DUCKER_MEM}\"/" "${DUCKER}"

# --- Gradle: build systemTestLibs (needed before ducker-ak up) -----------

echo "=== building apache/kafka systemTestLibs ==="
(cd "${KAFKA_CHECKOUT}" && ./gradlew -q systemTestLibs)

# --- ducker-ak: bring up cluster -----------------------------------------

echo "=== bringing up ducker cluster (${NUM_NODES} nodes) ==="
# Always teardown-then-up; a leftover cluster.json from a prior partial
# run produces invalid JSON on the next up.
(cd "${KAFKA_CHECKOUT}" && ./tests/docker/ducker-ak down || true)
rm -f "${KAFKA_CHECKOUT}/tests/docker/build/cluster.json" \
      "${KAFKA_CHECKOUT}/tests/docker/build/node_hosts"

LIBRDKAFKA_DIR="${LIBRDKAFKA_DIR}" \
    "${KAFKA_CHECKOUT}/tests/docker/ducker-ak" up -n "${NUM_NODES}"

# Always teardown on exit (success or failure).
cleanup() {
    echo "=== tearing down ducker cluster ==="
    (cd "${KAFKA_CHECKOUT}" && ./tests/docker/ducker-ak down) || true
}
trap cleanup EXIT

# --- run tests -----------------------------------------------------------

echo "=== running Ducktape tests ==="
# Quoting: ducker-ak test builds `docker exec ducker01 bash -c "..."`.
# Unquoted embedded quotes in test symbols break it; we pass simple
# filename paths and let the matrix run. --sample 1 to pick one variant
# per test (saves time; we can remove for full coverage later).
set +e
(cd "${KAFKA_CHECKOUT}" && \
 LIBRDKAFKA_DIR="${LIBRDKAFKA_DIR}" ./tests/docker/ducker-ak test \
    ${TESTS_TO_RUN} \
    -- --globals "/librdkafka/tests/kafkatest/globals.json" --sample 1)
TEST_EXIT=$?
set -e

# --- collect artifacts ---------------------------------------------------

ARTIFACT_DIR="${LIBRDKAFKA_DIR}/artifacts/ducktape-results"
mkdir -p "${ARTIFACT_DIR}"
docker cp ducker01:/opt/kafka-dev/results "${ARTIFACT_DIR}/" 2>/dev/null || \
    echo "(no results dir on ducker01)"

echo "=== run-system-tests.sh done, test_exit=${TEST_EXIT} ==="
exit ${TEST_EXIT}
