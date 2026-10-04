# zilf-hg — fixed AUR PKGBUILD for ZILF/ZAPF

Upstream AUR package: https://aur.archlinux.org/packages/zilf-hg
(maintainer: libele). This copy contains our fixes until they are
merged upstream. Suggested diff summary for the maintainer:

* Upstream ZILF (foss.heptapod.net/zilf/zilf) reworked `Build.proj`:
  the `Stage` target now **requires** `-p:RuntimeIdentifier`.
* Upstream migrated `net5.0 → net10.0` and now stages **self-contained
  AOT single-file binaries**; the old `cp -a bin/Release/net5.0/*`
  found nothing.
* Staged binary names are lowercase: `zilf`, `zapf`, `zilfpub`.
* `package()` now installs from
  `Package/Release/Stage/zilf-*-linux-x64/{bin,zillib,sample}`.
* `makedepends` gains `clang` (required for PublishAOT).
* Result: no dotnet runtime needed at install time; `zillib/` ships in
  `/usr/share/zilf/`.

Verified building ZILF 1.9 @ hg r1694.428d9a205a73 (2026-10-04):

```sh
makepkg -si            # installs /usr/bin/{zilf,zapf,zilfpub}
zilf --version         # -> 1.9
```

Used by `../../games/*/build.sh` (see `../../games/README.md`).
