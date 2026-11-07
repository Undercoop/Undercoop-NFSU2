# Public source releases

Copyright (C) 2026 batuhan. Original Undercoop documentation is licensed under
GNU GPL version 3 only, with LICENSE-7ZIP-EXCEPTION.txt.

For each mod/launcher patch:

1. Finish the changes and update both versions in src/CoopBuildVersion.h.
2. Run Release-Source.cmd --replace from this checkout. The exporter reads the
   version, creates backups/Undercoop-VERSION-source.zip and preserves an older
   same-version archive with a timestamp before replacing it.
3. Extract the new ZIP to a fresh verification directory. Run Build-Source.cmd
   there with Visual Studio x86 C++ tools and the Windows SDK.
4. Verify the manifest, component notices and public assets. Publish the source
   archive alongside the matching patch. Retain the original full source tree
   and modified dependency sources for each released binary.

The GitHub README template is release-assets/README.md; its version placeholder
is expanded by the exporter. Reviewed logo, diagram and launcher images live in
release-assets/docs/media and are exported alongside that README. Update these
files when a patch changes user-facing behavior or architecture.

The exporter includes only the mod, launcher, corresponding dependency source
and build files. It excludes the developer website/backend, credentials, game
data, careers, logs, Git history and unrelated compiled programs. Its scanner
provides a further release check for known credential formats.

The owner's existing build retains its private font resources. The exported
source uses Barlow for body text and Chakra Petch Bold for headings/overlay
text, under SIL OFL 1.1. The font copyright notices and full license texts are
included, also as resources of the exported ASI. The exported startup splash
uses the supplied Undercoop logo instead of game artwork with unverified
redistribution rights. No game logic is changed by these export substitutions.

The private font and startup image files are not in the public ZIP. Substituting
assets in source does not grant distribution permission for those assets in an
existing binary. A personal-font binary is not the public-font build. Obtain
the required font/art permissions before distributing a binary that uses them.

Original Undercoop code is GPL-3.0-only with the narrow 7-Zip permission. Bundled
third-party components retain their own licenses. Undercoop branding is covered
separately by TRADEMARKS.md and does not restrict GPL code rights.
