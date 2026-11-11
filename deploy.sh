#!/bin/sh
# The camera's settings, written under deploy/config/overlay/ (not in git).
# uvc/make-sdcard.sh copies them to the SD card, and the camera installs
# them over its root filesystem at boot.
#   ./deploy.sh wifi     the WiFi network (WPA or WPA2 personal)
#   ./deploy.sh passwd   root's password and your SSH public key
set -e
cd "$(dirname "$0")"
ETC=deploy/config/overlay/etc

ask() { # prompt variable [silent]
    while :; do
        printf '%s' "$1" >&2
        [ -z "$3" ] || stty -echo
        IFS= read -r REPLY
        [ -z "$3" ] || { stty echo; echo >&2; }
        [ -z "$REPLY" ] || break
    done
    eval "$2=\$REPLY"
}

case "$1" in
wifi)
    ask "Network name (SSID): " SSID
    ask "Password: " PSK silent
    printf 'Hidden network? [y/N] ' >&2
    read -r HIDDEN
    case "$HIDDEN" in y* | Y*) SCAN=1 ;; *) SCAN=0 ;; esac
    case "$SSID$PSK" in *'"'*) echo "a double quote in the name or password is not supported" >&2; exit 1 ;; esac
    [ ${#PSK} -ge 8 ] && [ ${#PSK} -le 63 ] || { echo "the password must have 8 to 63 characters" >&2; exit 1; }
    mkdir -p $ETC
    cat > $ETC/wpa_supplicant.conf <<CONF
ctrl_interface=/var/run/wpa_supplicant

network={
	ssid="$SSID"
	scan_ssid=$SCAN
	key_mgmt=WPA-PSK
	psk="$PSK"
}
CONF
    chmod 600 $ETC/wpa_supplicant.conf
    echo "$ETC/wpa_supplicant.conf written"
    ;;
passwd)
    ask "Password for root: " PASS silent
    ask "Again: " PASS2 silent
    [ "$PASS" = "$PASS2" ] || { echo "the passwords differ" >&2; exit 1; }
    mkdir -p $ETC deploy/config/overlay/root/.ssh
    # SHA-512 crypt: the camera's dropbear checks it with musl's crypt()
    printf 'root:%s:0:0:root:/root:/bin/sh\n' "$(printf '%s' "$PASS" | openssl passwd -6 -stdin)" > $ETC/passwd
    echo "$ETC/passwd written"
    for k in id_ed25519 id_ecdsa id_rsa; do
        [ -f ~/.ssh/$k.pub ] || continue
        cp ~/.ssh/$k.pub deploy/config/overlay/root/.ssh/authorized_keys
        chmod 600 deploy/config/overlay/root/.ssh/authorized_keys
        echo "~/.ssh/$k.pub is the camera's authorized key"
        exit 0
    done
    echo "no SSH public key in ~/.ssh: the camera will take the password only" >&2
    ;;
*)
    echo "usage: $0 wifi|passwd" >&2
    exit 2
    ;;
esac
