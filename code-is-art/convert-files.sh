#!/bin/sh

xxd -i sources.zip | sed -e 's/unsigned/static const unsigned/' > src/sources.zip.h
xxd -i nyan.png | sed -e 's/unsigned/static const unsigned/' > src/nyan.png.h
xxd -i default.png | sed -e 's/unsigned/static const unsigned/' > src/default.png.h
xxd -i index.html | sed -e 's/unsigned/static const unsigned/' > src/index.html.h
xxd -i config.html | sed -e 's/unsigned/static const unsigned/' > src/config.html.h

rm -rf code-is-art
mkdir code-is-art
cp -a src code-is-art

cp 00-README code-is-art
cp CMakeLists.txt code-is-art
cp convert-files.sh code-is-art
cp install.sh code-is-art
cp index.html code-is-art
cp config.html code-is-art
cp nyan.png code-is-art
cp default.png code-is-art

# macOS loves to add these everywhere
rm code-is-art/.DS_Store
rm code-is-art/src/.DS_store

# don't include sources recursively :)
rm code-is-art/src/sources.zip.h
rm sources.zip
zip -r sources.zip code-is-art
