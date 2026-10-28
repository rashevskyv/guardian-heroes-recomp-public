#!/bin/sh
# Copies the Switch build to the console over FTP and verifies it. Run in WSL.
#   switch/deploy.sh <console IP> [push|game|logs]   (default: push)
# push: gh.nro goes up under a temporary name, is renamed, downloaded again and compared by SHA-1 (the console
#       has gone offline mid-upload before); the staged shaders/ and the `deko3d` marker follow. The NACP
#       version is checked first (tools/nro_nacp.py): hbmenu shows it.
# game: mirrors $GH_GAME_DIR (default /root/gh/game: the extracted STFS, default.xex + media/) to sdmc:/switch/gh/game/ with lftp and
#       compares the byte count of the remote tree with the local one. Once per game version.
# logs: gh_log.txt and gh_rex_log.txt -> switch/logs/ (git-ignored).
# Paths: sdmc:/switch/gh/{gh.nro,shaders/,deko3d,game/}; FTP port 5000 (SWITCH_FTP_PORT).
set -e
cd "$(dirname "$0")/.."
IP=$1
ACTION=${2:-push}
[ -n "$IP" ] || { sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
PORT=${SWITCH_FTP_PORT:-5000}
URL="ftp://$IP:$PORT/switch/gh"
BUILD=${SWITCH_BUILD_DIR:-/root/gh/src/out/build/switch}
GAME=${GH_GAME_DIR:-/root/gh/game}
NRO=$BUILD/gh.nro
VERSION=$(grep -o 'GH_VERSION "[^"]*"' "$BUILD/gh_version.h" | cut -d'"' -f2)
case "$ACTION" in
    push)
        python3 tools/nro_nacp.py "$NRO" "$VERSION"
        mkdir -p switch/logs/elf/$VERSION && cp "$BUILD/gh.elf" switch/logs/elf/$VERSION/gh.elf  # keeps the symbols of every pushed build (console pc samples)
        local_sha=$(sha1sum "$NRO" | cut -c1-40)
        # lftp reconnects and resumes (the console's FTP resets long transfers now and then).
        LFTP="set net:max-retries 8; set net:reconnect-interval-base 3; set net:timeout 25"
        lftp -p "$PORT" -e "$LFTP; put -c -O /switch/gh/ $NRO -o gh.nro.tmp; bye" "$IP"
        rm -f /tmp/gh-readback.nro
        lftp -p "$PORT" -e "$LFTP; get -c /switch/gh/gh.nro.tmp -o /tmp/gh-readback.nro; bye" "$IP"
        tmp_sha=$(sha1sum /tmp/gh-readback.nro | cut -c1-40)
        if [ "$local_sha" != "$tmp_sha" ]; then
            echo "VERIFY FAILED (temp file): local $local_sha, console $tmp_sha" >&2
            exit 1
        fi
        # The console's FTP server refuses a rename onto an existing file: delete the old NRO first.
        curl -sS "ftp://$IP:$PORT/" -Q "-DELE /switch/gh/gh.nro" -o /dev/null 2>/dev/null || true
        curl -sS "ftp://$IP:$PORT/" -Q "-RNFR /switch/gh/gh.nro.tmp" -Q "-RNTO /switch/gh/gh.nro" -o /dev/null
        remote_size=$(curl -sS "ftp://$IP:$PORT/switch/gh/" | awk '$NF=="gh.nro" {print $5}')
        [ "$remote_size" = "$(stat -c %s "$NRO")" ] || { echo "VERIFY FAILED: size $remote_size" >&2; exit 1; }
        remote_sha=$local_sha  # the renamed file is the verified temp file
        lftp -p "$PORT" -e "mirror -R --delete --parallel=2 $BUILD/stage/shaders /switch/gh/shaders; put -O /switch/gh/ $BUILD/stage/deko3d; bye" "$IP" > /dev/null
        echo "Copied and verified gh.nro ($local_sha) on $IP, version $VERSION; shaders and the deko3d marker follow."
        ;;
    game)
        lftp -p "$PORT" -e "mirror -R --only-newer --parallel=3 $GAME /switch/gh/game; bye" "$IP"
        local_bytes=$(find $GAME -type f -printf '%s\n' | awk '{s+=$1} END{print s}')
        remote_bytes=$(lftp -p "$PORT" -e "du -sb /switch/gh/game; bye" "$IP" | awk '{print $1}' | tail -1)
        echo "game data: local $local_bytes bytes, console $remote_bytes bytes"
        [ "$local_bytes" = "$remote_bytes" ] || { echo "SIZE MISMATCH" >&2; exit 1; }
        ;;
    logs)
        mkdir -p switch/logs
        for log in gh_log.txt gh_rex_log.txt; do
            curl -sS -o "switch/logs/$log" "$URL/$log" && echo "switch/logs/$log" || true
        done
        ;;
    *)
        echo "Unknown action: $ACTION (push, game or logs)" >&2
        exit 1
        ;;
esac
