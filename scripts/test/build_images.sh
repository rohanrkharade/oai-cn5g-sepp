#!/usr/bin/env bash
# Build the requested upstream revisions without changing developer checkouts.
set -euo pipefail
export GIT_TERMINAL_PROMPT=0
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BUILD_DIR="${LBO_BUILD_DIR:-$ROOT_DIR/.lbo-build}"
NF_GIT_BASE="${NF_GIT_BASE:-https://github.com/openairinterface}"
NF_BRANCH="${NF_BRANCH:-lbo-roaming-support}"
AUSF_BRANCH="${AUSF_BRANCH:-develop}"
UPF_BRANCH="${UPF_BRANCH:-develop}"
SEPP_DEFAULT_REPOSITORY="$(git -C "$SCRIPT_DIR/../.." remote get-url origin 2>/dev/null || printf '%s' "$NF_GIT_BASE/oai-cn5g-sepp.git")"
SEPP_REPOSITORY="${SEPP_REPOSITORY:-$SEPP_DEFAULT_REPOSITORY}"
SEPP_BRANCH="${SEPP_BRANCH:-}"
UERANSIM_REPOSITORY="${UERANSIM_REPOSITORY:-https://github.com/rohanrkharade/UERANSIM.git}"
UERANSIM_BRANCH="${UERANSIM_BRANCH:-}"
BASE_IMAGE="${BASE_IMAGE:-ubuntu:jammy}"
TARGETPLATFORM="${TARGETPLATFORM:-linux/amd64}"
mode="${1:-build}"
case "$mode" in
    build|--check|--plan) ;;
    -h|--help)
        cat <<'HELP'
Usage: ./build_images.sh [--plan|--check]
Default: validate remote refs, build all eight core NFs, then build UERANSIM.
--plan: print source/branch/image choices without network access or mutation.
--check: check every remote branch without cloning, building, or deploying.
Overrides: NF_GIT_BASE, NF_BRANCH, AUSF_BRANCH, UPF_BRANCH, SEPP_REPOSITORY, SEPP_BRANCH,
UERANSIM_REPOSITORY, UERANSIM_BRANCH, LBO_BUILD_DIR, BASE_IMAGE, TARGETPLATFORM.
SEPP_REPOSITORY defaults to the origin of this SEPP checkout.
An unset SEPP_BRANCH or UERANSIM_BRANCH selects that repository's default branch.
Existing workspace checkouts and Docker volumes are never modified.
HELP
        exit 0 ;;
    *) echo "Unknown option: $mode" >&2; exit 2 ;;
esac
[[ $# -le 1 ]] || { echo 'Only one mode argument is supported' >&2; exit 2; }
nfs=(nrf udr udm ausf amf smf upf sepp)
components=("${nfs[@]}" ueransim)
declare -A urls branches revisions tags
for nf in "${nfs[@]}"; do
    urls[$nf]="$NF_GIT_BASE/oai-cn5g-$nf.git"
    branches[$nf]="$NF_BRANCH"
    tags[$nf]="oai-$nf:roaming-lbo"
done
branches[ausf]="$AUSF_BRANCH"; branches[upf]="$UPF_BRANCH"
urls[sepp]="$SEPP_REPOSITORY"; branches[sepp]="$SEPP_BRANCH"
urls[ueransim]="$UERANSIM_REPOSITORY"; branches[ueransim]="$UERANSIM_BRANCH"
tags[ueransim]='ueransim:roaming-lbo'
for component in "${components[@]}"; do
    printf '%-9s %-65s branch=%-18s image=%s\n' "$component" "${urls[$component]}" "${branches[$component]:-(remote default)}" "${tags[$component]}"
done
[[ $mode != --plan ]] || exit 0
command -v git >/dev/null
failed=0
for component in "${components[@]}"; do
    branch="${branches[$component]}"
    if [[ -z $branch ]]; then
        if ! listing=$(git ls-remote --symref "${urls[$component]}" HEAD); then
            echo "ERROR: cannot access ${urls[$component]}" >&2; failed=1; continue
        fi
        branch=$(awk '$1=="ref:" && $3=="HEAD" {sub("refs/heads/", "", $2); print $2}' <<< "$listing")
        revision=$(awk '$2=="HEAD" && $1!="ref:" {print $1}' <<< "$listing")
    else
        if ! listing=$(git ls-remote "${urls[$component]}" "refs/heads/$branch"); then
            echo "ERROR: cannot access ${urls[$component]}" >&2; failed=1; continue
        fi
        revision=$(awk 'NR==1 {print $1}' <<< "$listing")
    fi
    if [[ -z $branch || ! $revision =~ ^[0-9a-f]{40}$ ]]; then
        echo "ERROR: $component branch '${branch:-remote default}' is unavailable; no fallback branch will be built." >&2
        failed=1; continue
    fi
    branches[$component]="$branch"; revisions[$component]="$revision"
    printf 'Verified %s: %s @ %s\n' "$component" "$branch" "$revision"
done
[[ $failed == 0 ]] || { echo 'Source validation failed. Publish/provide the requested branches and retry. No images were built.' >&2; exit 1; }
[[ $mode != --check ]] || exit 0
command -v docker >/dev/null
docker info >/dev/null
run_dir="$BUILD_DIR/runs/$(date -u +%Y%m%dT%H%M%SZ)-$$"
mkdir -p "$BUILD_DIR/sources" "$run_dir"
printf 'component\trepository\tbranch\tcommit\timage\timage_id\n' > "$run_dir/manifest.tsv"
for component in "${components[@]}"; do
    # UERANSIM is deliberately last: a failed NF build stops the script first.
    if [[ $component == ueransim ]]; then repo_name=UERANSIM; else repo_name="oai-cn5g-$component"; fi
    source_dir="$BUILD_DIR/sources/$repo_name"
    if [[ -e $source_dir ]]; then
        [[ $(git -C "$source_dir" remote get-url origin) == "${urls[$component]}" ]] || { echo "Origin mismatch: $source_dir" >&2; exit 1; }
        [[ -z $(git -C "$source_dir" status --porcelain --untracked-files=all) ]] || { echo "Refusing to replace modified build sources: $source_dir" >&2; exit 1; }
    else
        git init "$source_dir"
        git -C "$source_dir" remote add origin "${urls[$component]}"
    fi
    git -C "$source_dir" fetch --depth 1 origin "${revisions[$component]}"
    git -C "$source_dir" checkout --detach "${revisions[$component]}"
    git -C "$source_dir" submodule sync --recursive
    git -C "$source_dir" submodule update --init --recursive
    args=(--build-arg "TARGETPLATFORM=$TARGETPLATFORM" --build-arg "BASE_IMAGE=$BASE_IMAGE"
          --label "org.opencontainers.image.source=${urls[$component]}"
          --label "org.opencontainers.image.revision=${revisions[$component]}"
          --tag "${tags[$component]}")
    if [[ $component == ueransim ]]; then
        dockerfile="$source_dir/Dockerfile"
        [[ -f $source_dir/tools/nr-ue-ping ]] || { echo 'UERANSIM revision must include tools/nr-ue-ping for the automatic traffic test.' >&2; exit 1; }
    else
        dockerfile="$source_dir/docker/Dockerfile.$component.ubuntu"
        args+=(--target "oai-$component")
    fi
    [[ -f $dockerfile ]] || { echo "Missing Dockerfile: $dockerfile" >&2; exit 1; }
    echo "Building $component from ${branches[$component]} @ ${revisions[$component]}"
    docker build "${args[@]}" -f "$dockerfile" "$source_dir" 2>&1 | tee "$run_dir/$component-build.log"
    image_id=$(docker image inspect --format '{{.Id}}' "${tags[$component]}")
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$component" "${urls[$component]}" "${branches[$component]}" "${revisions[$component]}" "${tags[$component]}" "$image_id" >> "$run_dir/manifest.tsv"
done
docker run --rm --entrypoint /bin/bash ueransim:roaming-lbo -c 'command -v nr-ue && command -v nr-gnb && command -v nr-cli && command -v nr-ue-ping && command -v ping && command -v timeout'
echo "All core NFs and then UERANSIM built successfully. Manifest: $run_dir/manifest.tsv"
echo "Deploy: cd '$SCRIPT_DIR' && docker compose -f docker-compose-basic-nrf-lbo-roaming.yaml up -d"
