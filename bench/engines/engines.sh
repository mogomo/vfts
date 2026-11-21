#!/usr/bin/env bash
# Install, start and stop the engines vtext is compared with, natively from their release
# tarballs (no Docker). Both bring their own Java.
#
#   bench/engines/engines.sh install            download Elasticsearch and OpenSearch, add the ICU plugin
#   bench/engines/engines.sh start es|os        start one engine (and wait until it answers)
#   bench/engines/engines.sh stop es|os|all
#   bench/engines/engines.sh status
#
# Settings (environment):
#   VTEXT_BENCH   install directory (default ~/vtext_bench)
#   ES_VERSION    default 9.5.6           OS_VERSION   default 3.10.0
#   ENGINE_HEAP   JVM heap, default 16g (the same for both engines)
#   ES_PORT       default 9200            OS_PORT      default 9201
#
# Both listen on 127.0.0.1 only, with security switched off: they are test servers on the
# benchmark machine, nothing else.
set -euo pipefail
B=${VTEXT_BENCH:-$HOME/vtext_bench}
ES_VERSION=${ES_VERSION:-9.5.6}
OS_VERSION=${OS_VERSION:-3.10.0}
HEAP=${ENGINE_HEAP:-16g}
ES_PORT=${ES_PORT:-9200}
OS_PORT=${OS_PORT:-9201}
ES_HOME=$B/elasticsearch-$ES_VERSION
OS_HOME=$B/opensearch-$OS_VERSION
mkdir -p "$B"

fetch() {   # url file
    [ -f "$B/$2" ] && return 0
    curl -fsSL --retry 3 --connect-timeout 30 --max-time 1800 -o "$B/$2.part" "$1"
    mv "$B/$2.part" "$B/$2"
}

install() {
    if [ ! -d "$ES_HOME" ]; then
        fetch "https://artifacts.elastic.co/downloads/elasticsearch/elasticsearch-$ES_VERSION-linux-x86_64.tar.gz" es.tar.gz
        tar xzf "$B/es.tar.gz" -C "$B"
        timeout 600 "$ES_HOME/bin/elasticsearch-plugin" install --batch analysis-icu
        cat >> "$ES_HOME/config/elasticsearch.yml" <<EOF
cluster.name: vtext-bench-es
node.name: es1
network.host: 127.0.0.1
http.port: $ES_PORT
transport.port: 9300
discovery.type: single-node
xpack.security.enabled: false
xpack.ml.enabled: false
path.data: $B/es-data
path.logs: $B/es-logs
indices.query.bool.max_clause_count: 100000
EOF
        printf -- "-Xms%s\n-Xmx%s\n" "$HEAP" "$HEAP" > "$ES_HOME/config/jvm.options.d/heap.options"
    fi
    if [ ! -d "$OS_HOME" ]; then
        fetch "https://artifacts.opensearch.org/releases/bundle/opensearch/$OS_VERSION/opensearch-$OS_VERSION-linux-x64.tar.gz" os.tar.gz
        tar xzf "$B/os.tar.gz" -C "$B"
        timeout 600 "$OS_HOME/bin/opensearch-plugin" install --batch analysis-icu
        cat >> "$OS_HOME/config/opensearch.yml" <<EOF
cluster.name: vtext-bench-os
node.name: os1
network.host: 127.0.0.1
http.port: $OS_PORT
transport.port: 9301
discovery.type: single-node
plugins.security.disabled: true
path.data: $B/os-data
path.logs: $B/os-logs
indices.query.bool.max_clause_count: 100000
EOF
        printf -- "-Xms%s\n-Xmx%s\n" "$HEAP" "$HEAP" > "$OS_HOME/config/jvm.options.d/heap.options"
    fi
    echo "installed: $ES_HOME, $OS_HOME"
}

port_of() { [ "$1" = es ] && echo "$ES_PORT" || echo "$OS_PORT"; }

start() {
    local e=$1 home pidf
    if [ "$e" = es ]; then home=$ES_HOME; else home=$OS_HOME; fi
    pidf=$B/$e.pid
    if [ -f "$pidf" ] && kill -0 "$(cat "$pidf")" 2>/dev/null; then echo "$e is running"; return 0; fi
    if [ "$e" = es ]; then
        "$home/bin/elasticsearch" -d -p "$pidf" > "$B/$e.out" 2>&1
    else
        OPENSEARCH_JAVA_OPTS="" "$home/bin/opensearch" -d -p "$pidf" > "$B/$e.out" 2>&1
    fi
    for i in $(seq 1 120); do
        if curl -fs -m 5 "http://127.0.0.1:$(port_of "$e")/_cluster/health?wait_for_status=yellow&timeout=5s" > /dev/null; then
            echo "$e started on port $(port_of "$e")"; return 0
        fi
        sleep 2
    done
    echo "$e did not start within 4 minutes; see $B/$e.out and the logs in $B/$e-logs" >&2
    return 1
}

stop() {
    local e=$1 pidf=$B/$1.pid
    [ -f "$pidf" ] || return 0
    local pid
    pid=$(cat "$pidf")
    kill "$pid" 2>/dev/null || true
    for i in $(seq 1 60); do kill -0 "$pid" 2>/dev/null || { rm -f "$pidf"; echo "$e stopped"; return 0; }; sleep 1; done
    kill -9 "$pid" 2>/dev/null || true
    rm -f "$pidf"
    echo "$e killed after 60 s"
}

case "${1:-}" in
    install) install ;;
    start) start "$2" ;;
    stop) if [ "$2" = all ]; then stop es; stop os; else stop "$2"; fi ;;
    status)
        for e in es os; do
            if curl -fs -m 5 "http://127.0.0.1:$(port_of $e)/" > /dev/null; then echo "$e: up on $(port_of $e)"; else echo "$e: down"; fi
        done ;;
    *) sed -n '2,20p' "$0"; exit 2 ;;
esac
