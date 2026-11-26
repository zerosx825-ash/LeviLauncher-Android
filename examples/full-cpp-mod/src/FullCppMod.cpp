// Lyrics overlay mod for LeviLauncher (Android).
//
// Papar lirik format LRC di skrin Minecraft ikut masa. Pemain mainkan lagu di
// Spotify sendiri, dan tekan butang "Start" dalam game serentak.
//
// Butang HUD (muncul bila modul "Lyrics Overlay" dihidupkan dalam Mod Menu):
//   Start  - mula lirik dari awal
//   Stop   - berhenti dan kosongkan skrin
//   Song   - tukar ke lagu seterusnya (senarai dalam Lyrics.hpp)
//   Lambat - lirik lambat 0.25 saat
//   Awal   - lirik awal 0.25 saat

#include "Lyrics.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <pl/Mod.hpp>
#include <pl/ModMenu.hpp>

namespace {

using Clock = std::chrono::steady_clock;
using pl::modmenu::ButtonEvent;
using pl::modmenu::DrawCommand;
using pl::modmenu::DrawCommandType;

constexpr const char *kModuleId = "lyrics_mod.hud";
constexpr const char *kTextSizeKey = "textSize";
constexpr const char *kPositionKey = "positionY";
constexpr const char *kStartButtonId = "lyrics_mod.start_button";
constexpr const char *kStopButtonId = "lyrics_mod.stop_button";
constexpr const char *kSongButtonId = "lyrics_mod.song_button";
constexpr const char *kLateButtonId = "lyrics_mod.late_button";
constexpr const char *kEarlyButtonId = "lyrics_mod.early_button";

constexpr double kOffsetStep = 0.25;
constexpr double kTailSeconds = 6.0;
constexpr auto kStatusDuration = std::chrono::milliseconds(2500);
constexpr auto kTickInterval = std::chrono::milliseconds(33);

struct LyricLine {
  double time = 0.0;
  std::string text;
};

int parseInt(std::string_view value, int fallback) {
  std::string text(value);
  char *end = nullptr;
  errno = 0;
  const long parsed = std::strtol(text.c_str(), &end, 10);
  if (end == text.c_str() || *end != '\0' || errno == ERANGE) {
    return fallback;
  }
  if (parsed < -1000000 || parsed > 1000000) {
    return fallback;
  }
  return static_cast<int>(parsed);
}

// "mm:ss.xx" -> saat. Pulangkan false untuk tag metadata seperti "ar:Nama".
bool parseStamp(std::string_view stamp, double &seconds) {
  const std::size_t colon = stamp.find(':');
  if (colon == std::string_view::npos || colon == 0) {
    return false;
  }
  const std::string minuteText(stamp.substr(0, colon));
  const std::string secondText(stamp.substr(colon + 1));
  if (secondText.empty()) {
    return false;
  }

  char *end = nullptr;
  const long minutes = std::strtol(minuteText.c_str(), &end, 10);
  if (end == minuteText.c_str() || *end != '\0' || minutes < 0) {
    return false;
  }
  end = nullptr;
  const double secs = std::strtod(secondText.c_str(), &end);
  if (end == secondText.c_str() || *end != '\0' || secs < 0.0) {
    return false;
  }
  seconds = static_cast<double>(minutes) * 60.0 + secs;
  return true;
}

std::string trim(std::string_view text) {
  const auto isSpace = [](char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
  };
  while (!text.empty() && isSpace(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && isSpace(text.back())) {
    text.remove_suffix(1);
  }
  return std::string(text);
}

// Menyokong beberapa timestamp pada satu baris: [00:10.00][01:30.00]teks
std::vector<LyricLine> parseLrc(std::string_view lrc) {
  std::vector<LyricLine> lines;
  std::size_t pos = 0;
  while (pos < lrc.size()) {
    std::size_t end = lrc.find('\n', pos);
    if (end == std::string_view::npos) {
      end = lrc.size();
    }
    const std::string_view raw = lrc.substr(pos, end - pos);
    pos = end + 1;

    std::vector<double> stamps;
    std::size_t i = 0;
    while (i < raw.size() && raw[i] == '[') {
      const std::size_t close = raw.find(']', i);
      if (close == std::string_view::npos) {
        break;
      }
      double seconds = 0.0;
      if (!parseStamp(raw.substr(i + 1, close - i - 1), seconds)) {
        break;
      }
      stamps.push_back(seconds);
      i = close + 1;
    }
    if (stamps.empty()) {
      continue;
    }

    const std::string text = trim(raw.substr(i));
    for (const double seconds : stamps) {
      lines.push_back(LyricLine{seconds, text});
    }
  }
  std::stable_sort(lines.begin(), lines.end(),
                   [](const LyricLine &a, const LyricLine &b) {
                     return a.time < b.time;
                   });
  return lines;
}

DrawCommand makeText(float x, float y, std::uint32_t color, float size,
                     const std::string &text) {
  DrawCommand command;
  command.type = DrawCommandType::Text;
  command.x = x;
  command.y = y;
  command.color = color;
  command.size = size;
  command.text = text;
  return command;
}

std::size_t countCodepoints(std::string_view text) {
  std::size_t count = 0;
  for (const char c : text) {
    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
      ++count;
    }
  }
  return count;
}

class LyricsMod {
public:
  static LyricsMod &instance() {
    static LyricsMod mod;
    return mod;
  }

  LyricsMod() : mSelf(*ll::mod::NativeMod::current()) {}

  [[nodiscard]] ll::mod::NativeMod &getSelf() const { return mSelf; }

  bool load() {
    {
      std::lock_guard lock(mStateMutex);
      selectSongLocked(0);
    }
    getSelf().getLogger().info("Loaded {} song(s)",
                               static_cast<int>(lyricsmod::kSongCount));
    return true;
  }

  bool enable() {
    auto &self = getSelf();

    const bool moduleRegistered =
        pl::modmenu::ModuleBuilder(kModuleId, "Lyrics Overlay")
            .modId(self.getId())
            .description("Show timed LRC lyrics on screen. Press Start "
                         "together with your music.")
            .defaultEnabled(true)
            .onToggle(onModuleToggle)
            .config(kTextSizeKey, "Text Size (%)",
                    pl::modmenu::ConfigType::SliderInt,
                    std::to_string(mTextSizePct.load()), "50", "200")
            .config(kPositionKey, "Vertical Position (%)",
                    pl::modmenu::ConfigType::SliderInt,
                    std::to_string(mPositionPct.load()), "0", "90")
            .onConfigChanged(onConfigChanged)
            .registerModule();

    bool registered = moduleRegistered;
    registered = registered && addButton(kStartButtonId, "Lyrics Start",
                                         "Start", 1.6f);
    registered =
        registered && addButton(kStopButtonId, "Lyrics Stop", "Stop", 1.6f);
    registered =
        registered && addButton(kSongButtonId, "Lyrics Next Song", "Song", 1.6f);
    registered = registered && addButton(kLateButtonId, "Lyrics Later",
                                         "Lambat", 1.8f);
    registered = registered && addButton(kEarlyButtonId, "Lyrics Earlier",
                                         "Awal", 1.6f);

    if (!registered) {
      self.getLogger().error("Failed to register Mod Menu module/buttons");
      unregisterAll();
      return false;
    }

    startThread();
    self.getLogger().info("Lyrics overlay enabled");
    return true;
  }

  bool disable() {
    shutdown();
    getSelf().getLogger().info("Disabled");
    return true;
  }

  bool unload() {
    shutdown();
    getSelf().getLogger().info("Unloaded");
    return true;
  }

private:
  struct View {
    std::string lyric;
    std::string status;
    bool operator==(const View &) const = default;
  };

  ll::mod::NativeMod &mSelf;

  std::mutex mStateMutex;
  std::vector<LyricLine> mLines;
  std::size_t mSongIndex = 0;
  bool mPlaying = false;
  Clock::time_point mStartTime{};
  double mOffset = 0.0;
  std::string mStatus;
  Clock::time_point mStatusUntil{};

  std::atomic_int mTextSizePct{100};
  std::atomic_int mPositionPct{12};
  std::atomic_bool mRedraw{false};
  std::atomic_bool mRunning{false};
  std::thread mThread;

  static bool addButton(const char *id, const char *displayName,
                        const char *label, float widthScale) {
    return pl::modmenu::ButtonBuilder(id, displayName)
        .moduleId(kModuleId)
        .label(label)
        .behavior(pl::modmenu::ButtonBehavior::Click)
        .sizeScale(widthScale, 1.0f)
        .onEvent(onButtonEvent)
        .registerButton();
  }

  static void onModuleToggle(std::string_view moduleId, bool enabled) {
    instance().handleModuleToggle(moduleId, enabled);
  }

  static void onConfigChanged(std::string_view moduleId, std::string_view key,
                              std::string_view value) {
    instance().handleConfigChanged(moduleId, key, value);
  }

  static void onButtonEvent(std::string_view buttonId, ButtonEvent event,
                            float) {
    instance().handleButtonEvent(buttonId, event);
  }

  // ---- state (panggil dengan mStateMutex dikunci) ----

  void setStatusLocked(std::string text) {
    mStatus = std::move(text);
    mStatusUntil = Clock::now() + kStatusDuration;
  }

  void selectSongLocked(std::size_t index) {
    mSongIndex = index % lyricsmod::kSongCount;
    mLines = parseLrc(lyricsmod::kSongs[mSongIndex].lrc);
    mPlaying = false;
  }

  std::string songLabelLocked() const {
    return "Lagu " + std::to_string(mSongIndex + 1) + "/" +
           std::to_string(lyricsmod::kSongCount) + ": " +
           std::string(lyricsmod::kSongs[mSongIndex].title);
  }

  void startLocked() {
    if (mLines.empty()) {
      setStatusLocked("Tiada lirik untuk lagu ini");
      return;
    }
    mPlaying = true;
    mStartTime = Clock::now();
    setStatusLocked("Mula - " + songLabelLocked());
  }

  void stopLocked() {
    mPlaying = false;
    setStatusLocked("Berhenti");
  }

  void nextSongLocked() {
    selectSongLocked(mSongIndex + 1);
    setStatusLocked(songLabelLocked());
  }

  void adjustOffsetLocked(double delta) {
    mOffset += delta;
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "Offset: %+.2fs", mOffset);
    setStatusLocked(buffer);
  }

  View computeViewLocked() {
    const auto now = Clock::now();
    View view;

    if (mPlaying) {
      const double elapsed =
          std::chrono::duration<double>(now - mStartTime).count() + mOffset;
      if (!mLines.empty() && elapsed > mLines.back().time + kTailSeconds) {
        mPlaying = false;
        setStatusLocked("Tamat");
      } else {
        const auto it = std::upper_bound(
            mLines.begin(), mLines.end(), elapsed,
            [](double value, const LyricLine &line) {
              return value < line.time;
            });
        const auto index = static_cast<std::ptrdiff_t>(it - mLines.begin()) - 1;
        if (index >= 0) {
          view.lyric = mLines[static_cast<std::size_t>(index)].text;
        }
      }
    }

    if (!mStatus.empty() && now >= mStatusUntil) {
      mStatus.clear();
    }
    view.status = mStatus;
    return view;
  }

  // ---- callback ----

  void handleModuleToggle(std::string_view moduleId, bool enabled) {
    if (moduleId != kModuleId) {
      return;
    }
    if (!enabled) {
      {
        std::lock_guard lock(mStateMutex);
        mPlaying = false;
        mStatus.clear();
      }
      mRedraw = true;
    }
  }

  void handleConfigChanged(std::string_view moduleId, std::string_view key,
                           std::string_view value) {
    if (moduleId != kModuleId) {
      return;
    }
    if (key == kTextSizeKey) {
      mTextSizePct = std::clamp(parseInt(value, mTextSizePct.load()), 50, 200);
    } else if (key == kPositionKey) {
      mPositionPct = std::clamp(parseInt(value, mPositionPct.load()), 0, 90);
    } else {
      return;
    }
    mRedraw = true;
  }

  void handleButtonEvent(std::string_view buttonId, ButtonEvent event) {
    if (event != ButtonEvent::Click) {
      return;
    }
    std::lock_guard lock(mStateMutex);
    if (buttonId == kStartButtonId) {
      startLocked();
    } else if (buttonId == kStopButtonId) {
      stopLocked();
    } else if (buttonId == kSongButtonId) {
      nextSongLocked();
    } else if (buttonId == kLateButtonId) {
      adjustOffsetLocked(-kOffsetStep);
    } else if (buttonId == kEarlyButtonId) {
      adjustOffsetLocked(+kOffsetStep);
    }
  }

  // ---- thread dan lukisan ----

  void startThread() {
    if (mRunning.exchange(true)) {
      return;
    }
    mThread = std::thread([this] { run(); });
  }

  void stopThread() {
    mRunning = false;
    if (mThread.joinable()) {
      mThread.join();
    }
  }

  void run() {
    View last;
    bool hasLast = false;
    while (mRunning) {
      View view;
      {
        std::lock_guard lock(mStateMutex);
        view = computeViewLocked();
      }
      const bool forced = mRedraw.exchange(false);
      if (forced || !hasLast || !(view == last)) {
        // Jangan kunci mStateMutex semasa hantar ke overlay supaya callback
        // butang tidak boleh tersekat.
        render(view);
        last = view;
        hasLast = true;
      }
      std::this_thread::sleep_for(kTickInterval);
    }
  }

  void render(const View &view) {
    std::vector<DrawCommand> commands;

    const auto surface = pl::modmenu::getHudSurfaceSize();
    const float screenW = surface.width > 0.0f ? surface.width : 1920.0f;
    const float screenH = surface.height > 0.0f ? surface.height : 1080.0f;

    if (!view.lyric.empty()) {
      float size = std::max(16.0f, screenH * 0.045f) *
                   (static_cast<float>(mTextSizePct.load()) / 100.0f);
      float textW = static_cast<float>(countCodepoints(view.lyric)) * size * 0.55f;
      const float maxWidth = screenW * 0.9f;
      if (textW > maxWidth) {
        size *= maxWidth / textW;
        textW = maxWidth;
      }
      const float x = (screenW - textW) * 0.5f;
      const float y = screenH * (static_cast<float>(mPositionPct.load()) / 100.0f);
      const float shadow = std::max(1.0f, size * 0.07f);

      // Bayang hitam supaya mudah dibaca di atas apa-apa latar.
      commands.push_back(
          makeText(x + shadow, y + shadow, 0xFF000000u, size, view.lyric));
      commands.push_back(makeText(x, y, 0xFFFFFFFFu, size, view.lyric));
    }

    if (!view.status.empty()) {
      const float size = std::max(14.0f, screenH * 0.03f);
      const float x = screenW * 0.02f;
      const float y = screenH * 0.88f;
      commands.push_back(
          makeText(x + 1.0f, y + 1.0f, 0xFF000000u, size, view.status));
      commands.push_back(makeText(x, y, 0xFFFFFFFFu, size, view.status));
    }

    pl::modmenu::submitDrawCommands(
        kModuleId, std::span<const DrawCommand>(commands.data(), commands.size()));
  }

  void unregisterAll() {
    pl::modmenu::unregisterButton(kStartButtonId);
    pl::modmenu::unregisterButton(kStopButtonId);
    pl::modmenu::unregisterButton(kSongButtonId);
    pl::modmenu::unregisterButton(kLateButtonId);
    pl::modmenu::unregisterButton(kEarlyButtonId);
    pl::modmenu::unregisterModule(kModuleId);
  }

  void shutdown() {
    stopThread();
    pl::modmenu::submitDrawCommands(kModuleId,
                                    std::span<const DrawCommand>());
    unregisterAll();
  }
};

} // namespace

PL_REGISTER_MOD(LyricsMod, LyricsMod::instance())
