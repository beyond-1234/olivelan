# Qt's own translation catalog

`qtbase_zh_CN.qm` is compiled from Qt's `qttranslations` module (source:
`translations/qtbase_zh_CN.ts`, Qt 6.7.3) with `lrelease`.

Copyright (C) The Qt Company Ltd. and Qt contributors.
Licensed under the GNU Lesser General Public License v3 (or GPL v3), matching the Qt
version Olivelan links against: <https://www.qt.io/licensing>.

It is embedded so that strings Qt itself provides - the standard dialog buttons
(OK / Cancel / Yes / No), line-edit context menus, message box titles and the
non-native file dialog - are localized together with Olivelan's own catalog.
Without it a Chinese UI still shows "OK"/"Cancel".

Only `zh_CN` is bundled: English is Qt's source language, so the catalog is not
needed for it.

To refresh it for a new Qt version:

```bash
curl -LO https://raw.githubusercontent.com/qt/qttranslations/v<version>/translations/qtbase_zh_CN.ts
lrelease qtbase_zh_CN.ts -qm qtbase_zh_CN.qm
```
