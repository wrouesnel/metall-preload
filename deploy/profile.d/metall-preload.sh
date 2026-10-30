# shellcheck shell=sh
# Serves nix's heap from Metall in login shells.
#
# deploy/install.sh --shells profile installs this as
# /etc/profile.d/metall-preload.sh with @LIB@ filled in. The library only
# activates in nix and nix-eval-jobs; everything else passes straight through.
if [ -r "@LIB@" ]; then
  case " ${LD_PRELOAD:-} " in
    *" @LIB@ "*) ;;
    *) export LD_PRELOAD="@LIB@${LD_PRELOAD:+ $LD_PRELOAD}" ;;
  esac
fi
