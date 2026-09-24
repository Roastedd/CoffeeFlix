# Homebrew App Store listing

`CoffeeFlix/` is the package for the [Homebrew App Store](https://hb-app.store/wiiu), in the format of
[fortheusers/wiiu-hbas-repo](https://github.com/fortheusers/wiiu-hbas-repo): `pkgbuild.json` with the
app's details and download, the icon (256×150), the banner (`screen.png`, 848×208) and screenshots
(1280×720).

## Submitting

1. Publish the release first: the package downloads
   `https://github.com/Roastedd/CoffeeFlix/releases/download/v2.0.0/coffeeflix.wuhb`.
2. Either fill in the form at [submit.fortheusers.org](https://submit.fortheusers.org), or fork
   `fortheusers/wiiu-hbas-repo`, copy this `CoffeeFlix` folder into its `packages/` folder and open a
   pull request.

## Updating

For a new version, change `version`, the release URL in `assets` and `changelog` in `pkgbuild.json`,
and send the same changes to `wiiu-hbas-repo`.

To check the package locally, run [spinarak](https://github.com/fortheusers/spinarak) from a folder
that contains only this package: `python3 spinarak.py`.
