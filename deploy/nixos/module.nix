# NixOS module: serves the heap of nix-daemon and of every user's nix from
# Metall.
#
#   imports = [ ./path/to/metall-preload/deploy/nixos/module.nix ];
#   services.metall-preload.enable = true;
{
  config,
  lib,
  pkgs,
  ...
}:
let
  cfg = config.services.metall-preload;
  lib' = "${cfg.package}/lib/libmetall_preload.so";
in
{
  options.services.metall-preload = {
    enable = lib.mkEnableOption "metall-preload for nix-daemon and user sessions";

    package = lib.mkOption {
      type = lib.types.package;
      default = pkgs.callPackage ./package.nix { };
      defaultText = lib.literalExpression "pkgs.callPackage ./package.nix { }";
      description = "The metall-preload package.";
    };

    daemon = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Preload the library into nix-daemon.";
    };

    sessions = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = ''
        Set LD_PRELOAD in every user session (through PAM), so that nix run
        by users is served from Metall too. The library only activates in
        nix and nix-eval-jobs; every other program passes straight through.
      '';
    };

    heapDir = lib.mkOption {
      type = lib.types.nullOr lib.types.str;
      default = null;
      example = "/var/tmp";
      description = "Where nix-daemon keeps its heaps. Null means /tmp.";
    };
  };

  config = lib.mkIf cfg.enable {
    systemd.services.nix-daemon.environment = lib.mkIf cfg.daemon (
      { LD_PRELOAD = lib'; } // lib.optionalAttrs (cfg.heapDir != null) { METALL_PRELOAD_DIR = cfg.heapDir; }
    );
    environment.sessionVariables = lib.mkIf cfg.sessions { LD_PRELOAD = lib'; };
  };
}
