#!/usr/bin/env python3
#
# Start a Kafka cluster with fixed ports for Docker-based Windows testing.
#
# This script runs INSIDE a Linux Docker container. It:
# 1. Monkey-patches trivup's TcpPortAllocator to use a fixed port base
#    (so Docker port mapping can be set up before the container starts)
# 2. Starts a KRaft Kafka cluster via trivup
# 3. Writes the client config (test.conf) to a mounted volume
# 4. Creates a ready signal file
# 5. Waits for SIGTERM (from docker stop)
#
# Usage (inside Docker):
#   python3 start-kafka-docker.py --version 4.2.0 --cpversion 8.0.0
#

import os
import sys
import signal
import time
import argparse

# Monkey-patch TcpPortAllocator BEFORE importing KafkaCluster,
# so that all port allocations use a fixed base port.
# This allows Docker port mapping (-p 19092:19092) to work since
# the PLAINTEXT port is known ahead of time.
import trivup.trivup as _trivup

PLAINTEXT_PORT = 19092

_original_next = _trivup.TcpPortAllocator.next
_next_port = [PLAINTEXT_PORT]


def _fixed_port_next(self, app, port_base=None):
    if port_base is None:
        port_base = _next_port[0]
    result = _original_next(self, app, port_base=port_base)
    _next_port[0] = result + 1
    return result


_trivup.TcpPortAllocator.next = _fixed_port_next

from trivup.clusters.KafkaCluster import KafkaCluster  # noqa: E402


def main():
    parser = argparse.ArgumentParser(
        description='Start Kafka cluster for Docker-based Windows testing')
    parser.add_argument('--version', type=str, default='4.2.0',
                        help='Apache Kafka version')
    parser.add_argument('--cpversion', type=str, default='8.0.0',
                        help='Confluent Platform version')
    parser.add_argument('--conf-output', type=str,
                        default='test.conf',
                        help='Path to write client config file')
    parser.add_argument('--ready-file', type=str,
                        default='.kafka-ready',
                        help='Path to write ready signal file')
    parser.add_argument('--brokers', type=int, default=3,
                        help='Number of Kafka brokers')
    parser.add_argument('--conf', type=str, default='',
                        help='Broker configuration as a JSON array of strings')
    args = parser.parse_args()

    import json
    broker_conf = json.loads(args.conf) if args.conf else []

    print('Starting Kafka cluster (version={}, cp_version={}, brokers={})'.format(
        args.version, args.cpversion, args.brokers))

    kc = KafkaCluster(
        kraft=True,
        broker_cnt=args.brokers,
        version=args.version,
        cp_version=args.cpversion,
        broker_conf=broker_conf,
    )

    print('Waiting for cluster to become operational...')
    kc.wait_operational(timeout=120)

    kc.write_client_conf(args.conf_output)
    print('Client config written to {}'.format(args.conf_output))
    print('Bootstrap servers: {}'.format(kc.bootstrap_servers))

    with open(args.ready_file, 'w') as f:
        f.write(kc.bootstrap_servers + '\n')
    print('Ready signal written to {}'.format(args.ready_file))

    # Wait for SIGTERM (from docker stop)
    running = True

    def handler(sig, frame):
        nonlocal running
        running = False

    signal.signal(signal.SIGTERM, handler)

    print('Kafka cluster running. Waiting for SIGTERM...')
    while running:
        time.sleep(1)

    print('Stopping cluster...')
    kc.stop()


if __name__ == '__main__':
    main()
