#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$repo_root"

bad=0

# Deployment source exports deliberately contain no .git directory. Their
# tracked-file hygiene was checked in the local repository before transfer.
# Avoid invoking Git on the remote test host while retaining an artifact check.
if [ "${GOATCORE_SOURCE_EXPORT:-0}" = 1 ]; then
    if find src include tests tools -type f \( -name '*.o' -o -name '*.d' \) -print -quit | grep -q .; then
        echo "[hygiene-check] source export contains object/dependency artifacts" >&2
        exit 1
    fi
    echo "[hygiene-check] source export artifacts ok; local Git hygiene checked separately"
    exit 0
fi
allowed_asset() {
    case "$1" in
        assets/README.txt|assets/registry_packets_26_1_1.bin|assets/tags_packet_26_1_1.bin|assets/chunk_0_0_26_1_1.bin)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

flag_bad() {
    echo "[hygiene-check] $1: $2" >&2
    bad=1
}

while IFS= read -r path; do
    case "$path" in
        *.o|*.d|mc_server|mc_recorder|mc_anvil_dump|mc_gen_bench|test_*)
            flag_bad "tracked build artifact" "$path"
            ;;
        .codex|world/*|data/*|mc_vania_asset/*|"erreur reseau"/*|llm/*|reports/*)
            flag_bad "tracked local/raw data" "$path"
            ;;
        PLAN*.md|REPORT*.md)
            flag_bad "tracked AI/report note" "$path"
            ;;
        docs/*:Zone.Identifier|docs/*.tmp|docs/*.bak)
            flag_bad "tracked temporary documentation artifact" "$path"
            ;;
        assets/*)
            if ! allowed_asset "$path"; then
                flag_bad "tracked non-minimal asset" "$path"
            fi
            ;;
    esac
done <<EOF
$(git ls-files)
EOF

status=$(git status --short --untracked-files=all --ignored=no)
if printf '%s\n' "$status" | grep -E '^\?\? "?changed,|^\?\? "?not staged for commit:|^\?\? "?tatus"?$|^\?\? "?erreur reseau/|^\?\? .*\.o$|^\?\? .*\.d$|^\?\? "?world/|^\?\? "?data/|^\?\? "?mc_vania_asset/' >/dev/null 2>&1; then
    printf '%s\n' "$status" | grep -E '^\?\? "?changed,|^\?\? "?not staged for commit:|^\?\? "?tatus"?$|^\?\? "?erreur reseau/|^\?\? .*\.o$|^\?\? .*\.d$|^\?\? "?world/|^\?\? "?data/|^\?\? "?mc_vania_asset/' >&2 || true
    flag_bad "untracked local/raw artifact" "see entries above"
fi

if [ "$bad" -ne 0 ]; then
    echo "[hygiene-check] failed" >&2
    exit 1
fi

echo "[hygiene-check] ok"
