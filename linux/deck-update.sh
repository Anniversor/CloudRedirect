#!/usr/bin/env bash
# cr-deck-update.sh -- keep a Steam Deck (or any SLSsteam + headcrab setup) on
# the Anniversor fork of CloudRedirect.
#
# What it does, in order:
#   1. points the user-level "cloudredirect" Flatpak remote at the fork's repo
#      (https://anniversor.github.io/CloudRedirect) if it is not there yet;
#   2. updates (or installs) the CloudRedirect Flatpak from it;
#   3. unless --skip-headcrab: runs the headcrab updater (SLSsteam + pinned Steam
#      client, https://github.com/Deadboy666/h3adcr-b) with its three
#      CloudRedirect sources rewritten to the fork, because the unpatched script
#      overwrites ~/.local/share/CloudRedirect/cloud_redirect.so with upstream's
#      build on every run;
#   4. copies the Flatpak's bundled cloud_redirect.so and cloud_redirect_cli into
#      ~/.local/share/CloudRedirect (exactly what the app's Update button does),
#      so Steam loads the fork's library on its next start;
#   5. installs itself as ~/.local/bin/cr-deck-update.sh plus a
#      "CloudRedirect (fork) Updater" entry in the application menu, to be used
#      instead of the "Headcrab Updater" entry from now on.
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/Anniversor/CloudRedirect/master/linux/deck-update.sh | bash
#   cr-deck-update.sh [--skip-headcrab] [--install-only]
#
# The whole script is parsed before anything runs (everything lives in
# functions and main is called on the last line), so a truncated download
# cannot execute half of it.
set -uo pipefail

APP_ID="org.cloudredirect.CloudRedirect"
REMOTE_NAME="cloudredirect"
FORK_PAGES="https://anniversor.github.io/CloudRedirect"
FORK_REPO_URL="$FORK_PAGES/repo"
FORK_FLATPAKREPO="$FORK_PAGES/cloudredirect.flatpakrepo"
FORK_RELEASES="https://github.com/Anniversor/CloudRedirect/releases/latest/download"
SELF_URL="https://raw.githubusercontent.com/Anniversor/CloudRedirect/master/linux/deck-update.sh"
HEADCRAB_URL="https://raw.githubusercontent.com/Deadboy666/h3adcr-b/refs/heads/main/headcrab.sh"
CR_DIR="$HOME/.local/share/CloudRedirect"
BIN_DIR="$HOME/.local/bin"
DESKTOP_DIR="$HOME/.local/share/applications"

skip_headcrab=0
install_only=0

log()  { printf '\n\033[1;36m[cr-deck-update]\033[0m %s\n' "$*"; }
warn() { printf '\n\033[1;33m[cr-deck-update] warning:\033[0m %s\n' "$*"; }
die()  { printf '\n\033[1;31m[cr-deck-update] error:\033[0m %s\n' "$*" >&2; exit 1; }

# "2.6.5.6+1a2b3c4" (fork) or "2.6.5+1a2b3c4" (upstream). grep -a needs no binutils.
so_version() {
    grep -a -o -m1 -E '[0-9]+\.[0-9]+\.[0-9]+(\.[0-9]+)?\+([0-9a-f]{7}(-dirty)?|unknown)' "$1" 2>/dev/null || echo unknown
}

parse_args() {
    local arg
    for arg in "$@"; do
        case "$arg" in
            --skip-headcrab) skip_headcrab=1 ;;
            --install-only) install_only=1 ;;
            -h|--help) sed -n '2,25p' "${BASH_SOURCE[0]:-/dev/null}"; exit 0 ;;
            *) die "unknown option: $arg (use --skip-headcrab or --install-only)" ;;
        esac
    done
}

ensure_remote() {
    local url
    url=$(flatpak remotes --user --columns=name,url 2>/dev/null | awk -v n="$REMOTE_NAME" '$1==n{print $2}')
    if [ "$url" = "$FORK_REPO_URL" ]; then
        log "Flatpak remote '$REMOTE_NAME' already points at the fork"
        return 0
    fi
    if [ -n "$url" ]; then
        log "Flatpak remote '$REMOTE_NAME' points at $url; switching it to the fork"
        flatpak remote-delete --user --force "$REMOTE_NAME" || die "could not remove the old '$REMOTE_NAME' remote"
    else
        log "adding the fork's Flatpak remote as '$REMOTE_NAME'"
    fi
    flatpak remote-add --user "$REMOTE_NAME" "$FORK_FLATPAKREPO" || die "could not add the fork's remote"
    # The app's own update check reads the remote's appstream data
    # ('flatpak remote-info' Version), which a fresh remote does not have yet.
    flatpak update --user --appstream "$REMOTE_NAME" >/dev/null 2>&1 || warn "could not refresh appstream data for '$REMOTE_NAME'"
}

update_flatpak() {
    if flatpak info --user "$APP_ID" >/dev/null 2>&1; then
        log "updating $APP_ID from '$REMOTE_NAME'"
        flatpak update --user -y --noninteractive "$APP_ID" || warn "flatpak update failed; continuing with the installed version"
    else
        log "installing $APP_ID from '$REMOTE_NAME'"
        flatpak install --user -y --noninteractive "$REMOTE_NAME" "$APP_ID" || die "flatpak install failed"
    fi
    local info
    info=$(flatpak info --user "$APP_ID" 2>/dev/null)
    log "installed: $(printf '%s\n' "$info" | awk -F': *' '/^ *Version:/{print $2}') from $(printf '%s\n' "$info" | awk -F': *' '/^ *Origin:/{print $2}')"
}

run_headcrab() {
    local script patched rc
    script=$(mktemp) || die "mktemp failed"
    curl -fsSL "$HEADCRAB_URL" -o "$script" || { rm -f "$script"; die "could not download the headcrab script"; }
    # Rewrite headcrab's three CloudRedirect sources to the fork. If upstream
    # renames the variables the script just runs unpatched, and
    # redeploy_from_flatpak puts the fork's library back afterwards anyway.
    sed -i \
        -e "s|^\([[:space:]]*CloudRedirectLib=\).*|\1\"$FORK_RELEASES/cloud_redirect.so\"|" \
        -e "s|^\([[:space:]]*CloudRedirectCLI=\).*|\1\"$FORK_RELEASES/cloud_redirect_cli\"|" \
        -e "s|^\([[:space:]]*cloudredirect=\).*|\1\"$FORK_FLATPAKREPO\"|" \
        "$script"
    patched=$(grep -c 'Anniversor' "$script")
    if [ "$patched" -lt 3 ]; then
        warn "only $patched of 3 headcrab CloudRedirect URLs could be rewritten (did upstream change the script?)"
    fi
    log "running headcrab (SLSsteam / Steam client updater) with CloudRedirect sources pointed at the fork"
    bash "$script"
    rc=$?
    rm -f "$script"
    [ "$rc" -eq 0 ] || warn "headcrab exited with status $rc"
}

redeploy_from_flatpak() {
    local base dir="" so cli before
    for base in "$HOME/.local/share/flatpak" "/var/lib/flatpak"; do
        if [ -f "$base/app/$APP_ID/current/active/files/share/cloud_redirect/cloud_redirect.so" ]; then
            dir="$base/app/$APP_ID/current/active/files/share/cloud_redirect"
            break
        fi
    done
    [ -n "$dir" ] || { warn "$APP_ID is not installed; nothing to deploy"; return 0; }
    so="$dir/cloud_redirect.so"
    cli="$dir/cloud_redirect_cli"
    mkdir -p "$CR_DIR"
    if [ -f "$CR_DIR/cloud_redirect.so" ] && cmp -s "$so" "$CR_DIR/cloud_redirect.so"; then
        log "deployed cloud_redirect.so already matches the Flatpak ($(so_version "$so"))"
    else
        before="none"
        [ -f "$CR_DIR/cloud_redirect.so" ] && before=$(so_version "$CR_DIR/cloud_redirect.so")
        install -m 755 "$so" "$CR_DIR/cloud_redirect.so" || die "could not write $CR_DIR/cloud_redirect.so"
        log "deployed cloud_redirect.so $(so_version "$so") (was $before) to $CR_DIR; restart Steam to load it"
    fi
    if [ -f "$cli" ] && ! cmp -s "$cli" "$CR_DIR/cloud_redirect_cli" 2>/dev/null; then
        install -m 755 "$cli" "$CR_DIR/cloud_redirect_cli" || warn "could not update cloud_redirect_cli"
    fi
}

install_self() {
    local target="$BIN_DIR/cr-deck-update.sh" src="${BASH_SOURCE[0]:-}"
    mkdir -p "$BIN_DIR" "$DESKTOP_DIR"
    if [ -n "$src" ] && [ -f "$src" ]; then
        # Never overwrite the copy that is currently running.
        if [ "$(realpath "$src")" != "$(realpath -m "$target")" ]; then
            install -m 755 "$src" "$target" || warn "could not install $target"
        fi
    else
        # Piped through bash: fetch a clean copy for the menu entry.
        if curl -fsSL "$SELF_URL" -o "$target.tmp"; then
            install -m 755 "$target.tmp" "$target" || warn "could not install $target"
        else
            warn "could not download $SELF_URL for the menu entry"
        fi
        rm -f "$target.tmp"
    fi
    cat > "$DESKTOP_DIR/cloudredirect-fork-update.desktop" <<EOF
[Desktop Entry]
Version=1.0
Type=Application
Name=CloudRedirect (fork) Updater
Comment=Update CloudRedirect from the Anniversor fork, run headcrab, redeploy the library
Exec=$target
Icon=org.cloudredirect.CloudRedirect
Terminal=true
Categories=Utility;
EOF
    update-desktop-database "$DESKTOP_DIR" 2>/dev/null || true
    log "installed $target and the 'CloudRedirect (fork) Updater' menu entry"
}

main() {
    parse_args "$@"
    log "CloudRedirect fork updater"
    command -v flatpak >/dev/null 2>&1 || die "flatpak not found"
    if [ "$install_only" -eq 1 ]; then
        install_self
        return 0
    fi
    ensure_remote
    update_flatpak
    if [ "$skip_headcrab" -eq 0 ]; then
        run_headcrab
    fi
    redeploy_from_flatpak
    install_self
    log "done. Restart Steam if a library was (re)deployed above."
}

main "$@"
