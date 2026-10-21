ESP-IDF v5.3.2 の `components/fatfs` をコピーしたもの（src, diskio, port, vfs, CMakeLists.txt, Kconfig, project_include.cmake）。ライセンス表記は各ファイルのまま。

変更点:

- `src/ffconf.h` の `FF_FS_EXFAT` を 1 にした。手持ちのexFATのmicroSDをフォーマットせずに使うため。LFNは `CONFIG_FATFS_LFN_HEAP=y`
- Kconfig に無いとき `FF_USE_LABEL` を 0/1 で定義するようにした。exFATのコードが式の中で使っていて、未定義だとコンパイルエラーになる

IDF本体は変更していない。ホスト側のイメージ生成ツールはIDFのオリジナルを使う。
