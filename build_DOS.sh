for f in libfaad/*.c; do
  i586-pc-msdosdjgpp-gcc -O2 -march=i586 \
    -I. -Iinclude -Ilibfaad \
    -DPACKAGE_VERSION=\"2.11.2\" \
    -DHAVE_LRINTF \
    -c "$f" -o "$(basename ${f%.c}.o)"
done

i586-pc-msdosdjgpp-ar rcs libfaad.a *.o
rm *.o
