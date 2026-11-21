# Edax icons

The "big mouth" mark: a blue-to-green ball with fangs. Every PNG, ICO and ICNS file is generated from the SVGs in `svg/`, so change those and regenerate the rest.

| Colour | Hex | Used for |
|---|---|---|
| Blue | `#3B9EFF` | Source files (`.ed`), start of the gradient |
| Green | `#2ED3A0` | Header files (`.edh`), end of the gradient |
| Dark gray | `#1C1F24` | Backgrounds, the mouth |

## Where each file goes

| Folder | Contents | Use |
|---|---|---|
| `svg/` | Master files: mark, logo, macOS logo, source file, header file, compiler, wordmark, lockups (light and dark) | Source for everything else; the docs website |
| `png/` | Every icon at 16, 24, 32, 48, 64, 128, 256, 512 and 1024 px | Anywhere a bitmap is needed |
| `github/` | `social-preview.png` (1280×640), lockups for the README, `favicon.ico` / `favicon.svg` | Repository and website |
| `vscode/` | `icons/ed.svg`, `icons/edh.svg`, `images/icon.png`, `package.json.fragment.json` | The VS Code extension |
| `windows/` | `edax.ico`, `edax-source.ico`, `edax-header.ico`, `edaxc.ico` | Installer, file associations, `edaxc.exe` resource |
| `macos/` | `edax.icns`, `edax-source.icns`, `edax-header.icns` | App bundle and document types |
| `linux/` | hicolor icon theme, `mime/packages/edax.xml`, `install.sh` | Packages (.deb/.rpm) or a per-user install |

## GitHub

**README header** that switches with the viewer's theme:

```html
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="icons/github/edax-lockup-dark.svg">
  <img alt="Edax" src="icons/github/edax-lockup-light.svg" width="360">
</picture>
```

**Social preview:** upload `github/social-preview.png` under *Settings → General → Social preview*.

**Language colour:** GitHub only shows a language colour and name for languages listed in GitHub Linguist, which requires the language to be used in many public repositories. Until then, `.gitattributes` can only map files to languages Linguist already knows.

## VS Code extension

Merge `package.json.fragment.json` into the extension's `package.json`, and copy `icons/` and `images/` next to it.

- `contributes.languages[].icon` gives `.ed` files a blue icon and `.edh` files a green icon. VS Code shows these in any file-icon theme that has no icon of its own for the language. This is better than shipping a full file-icon theme, which would replace all of the user's other icons.
- `icon` is the 256 px Marketplace icon.
- The two language ids (`edax`, `edax-header`) can share one TextMate grammar.

## Windows

- Use `edaxc.ico` as the icon resource of `edaxc.exe`.
- In the installer, register the file types with the source and header icons, for example in Inno Setup or WiX:
  - `HKCR\.ed` → `Edax.Source`, with `DefaultIcon` = `edax-source.ico`
  - `HKCR\.edh` → `Edax.Header`, with `DefaultIcon` = `edax-header.ico`

## macOS

- `edax.icns` is the app icon. Its body is inset, like other macOS icons.
- `edax-source.icns` and `edax-header.icns` go into the app's `Info.plist` under `CFBundleDocumentTypes`, with UTIs that declare `.ed` and `.edh`.

## Linux

Run `linux/install.sh` to install for the current user. It copies the icons into `~/.local/share/icons/hicolor` and registers the MIME types `text/x-edax-source` (`*.ed`) and `text/x-edax-header` (`*.edh`).

A distribution package installs the same files to `/usr/share/icons/hicolor` and `/usr/share/mime/packages`.
