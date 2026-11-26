#pragma once

// Senarai lagu untuk mod lirik.
//
// Cara tambah lagu:
//   1. Dapatkan lirik format LRC (contoh dari lrclib.net) untuk lagu anda.
//   2. Tambah satu blok { "Tajuk", R"LRC( ...lirik LRC... )LRC" } di bawah.
//   3. Simpan (commit). GitHub Actions akan build mod baru secara automatik.
//
// Format LRC: [minit:saat.perseratus] teks lirik
// Baris metadata seperti [ar:Artis] atau [ti:Tajuk] diabaikan.
// Letak baris kosong ([03:30.00]) di hujung supaya lirik terakhir hilang.

#include <cstddef>
#include <string_view>

namespace lyricsmod {

struct Song {
  std::string_view title;
  std::string_view lrc;
};

inline constexpr Song kSongs[] = {
    {"Lagu Contoh", R"LRC(
[ti:Lagu Contoh]
[00:02.00] Ini baris lirik pertama
[00:05.50] Ini baris kedua
[00:09.00] Baris ketiga muncul di sini
[00:13.00]
[00:15.00] Tamat contoh
[00:19.00]
)LRC"},

    // {"Tajuk Lagu Kedua", R"LRC(
    // [00:00.00] tampal lirik LRC di sini
    // )LRC"},
};

inline constexpr std::size_t kSongCount = sizeof(kSongs) / sizeof(kSongs[0]);
static_assert(kSongCount > 0, "Tambah sekurang-kurangnya satu lagu");

} // namespace lyricsmod
