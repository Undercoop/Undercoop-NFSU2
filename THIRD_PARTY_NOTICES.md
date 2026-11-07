# Third-party components

The GPLv3 grant for original Undercoop code preserves the following component
licenses and notices.

| Component | Terms and included notices |
| --- | --- |
| ENet | MIT. See src/third_party/enet/LICENSE. |
| miniz | MIT. See src/third_party/miniz/LICENSE. |
| Nikki | MIT. See research/nikki-binary-audit/LICENSE and src/binary_engine/NOTICE.txt. |
| CoreExtensions | MIT. See research/coreextensions-binary-audit/LICENSE.md and src/binary_engine/NOTICE.txt. |
| 7-Zip decoder | LGPL-2.1-or-later, applicable BSD terms and the unRAR restriction. See src/third_party/7zip/DOC/License.txt and copying.txt. The additional Undercoop permission is in LICENSE-7ZIP-EXCEPTION.txt. |

Public source exports additionally include the following unmodified font files.
These fonts retain SIL Open Font License 1.1 and are not relicensed under GPL.

| Font | Copyright and included license |
| --- | --- |
| Barlow, Regular/Bold/Italic/BoldItalic | Copyright 2017 The Barlow Project Authors. See src/assets/launcher/fonts/barlow/OFL.txt in the public export. |
| Chakra Petch Bold | Copyright 2018 The Chakra Petch Project Authors. See src/assets/launcher/fonts/chakrapetch/OFL.txt in the public export. |

Font provenance and SHA-256 hashes are in the exported
src/assets/launcher/fonts/PROVENANCE.json. Public exports replace the private
Sport and ZT Nature resources; those original fonts are not included.

The source for the separately loaded 7-Zip decoder, including the Undercoop
allocation-budget changes, is in research/undercoop-7zip2605-src.tar.xz.
The build and replacement instructions are in src/assets/ArchiveEngineSource.txt.
Changes within that decoder remain under the component's applicable terms.

The files under src/binary_engine/nikki_safe retain their existing MIT
provenance notices. The source-built Binary engine libraries are supplied with
their corresponding source and the notices identified above.

The optional Microsoft .NET runtime is downloaded separately after user
approval. Its own LICENSE.txt and ThirdPartyNotices.txt govern that runtime.
