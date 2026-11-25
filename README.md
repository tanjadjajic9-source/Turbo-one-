# Image Trainer for Xbox (the converter)

This version of the app does not train a network. It turns each of your pictures into a **painting made of brush strokes**, on the Xbox's graphics chip, and keeps every painting as a very small **card of dials**. You can then put each original next to its painting and see how close the strokes got.

The Xbox shows a controller-friendly screen on the TV. Your phone (or any browser on the same Wi-Fi) shows a full control page.

This is **Stage A** of a three-stage plan. Stage B (a library with a replay slider, a bigger live view on the TV, a 1080p render and downloads) and Stage C (the Painter, which paints from the cards) are not in this version on purpose: they get built only after this stage has been tried on a real Xbox.

## First start, in three steps

1. Open the app on the Xbox. The **Phone** screen shows a web address and a square code.
2. On your phone, on the same Wi-Fi, scan the code with the camera or type the address into the browser.
3. On the phone page: **Pictures** > **Add pictures**, then **Convert** > pick a detail level > **Convert**.

Nothing ever starts converting by itself. Convert starts only when you press it.

## What the converter does

For every picture the graphics chip starts from the picture's average colour and then paints brush strokes one after the other, in three flights:

| Flight | Share of the strokes | What it paints |
| --- | --- | --- |
| **Wash** | 20 % | big soft strokes that lay down the broad colours |
| **Form** | 30 % | medium strokes that build the main shapes |
| **Detail** | 50 % | small crisp strokes that follow the edges |

For each stroke the chip tries **64 candidate strokes** where the painting is still wrong, gives every candidate its best colour, keeps the best one, nudges it **3 more times** to make it a little better, and paints it. A stroke is only kept if it lowers the error by a small, fixed amount; otherwise it is written as an empty stroke (it costs nothing but its place on the card).

The result is a **card**: 3 numbers for the background colour, then 12 numbers for every stroke (where, how big, how stretched, which way, how soft, how see-through, and its colour). Every number is a whole number from 0 to 127. A card of 1,500 strokes is 18,003 bytes. A card does not depend on a picture size: it can be painted again at any size from the dials alone.

The same picture, detail level and seed give the same card (the self-test checks this on your console every time the app starts).

### Detail levels

| Level | Strokes per picture | The chip looks at the picture at |
| --- | --- | --- |
| **Low** | 500 | 64 by 64 pixels |
| **Medium** | 1,500 | 96 by 96 pixels |
| **High** | 3,000 | 128 by 128 pixels |
| **Ultra** | 6,000 | 128 by 128 pixels |

More strokes give a closer painting and take proportionally longer. Pictures are stored at 128 by 128 pixels.

### The numbers you will see

- **Speed** is strokes per second, counted over everything the chip painted. This is the number to compare between detail levels and between runs.
- **Leftover error** is how far the painting is from the picture it was made from: the root-mean-square difference on a scale of 0 to 255, averaged over the pictures done so far. **Lower is better.** 0 would be a perfect copy.
- **Time left** is an estimate from the speed so far.

## Screen Sleep

Converting is quickest when the Xbox does nothing else. With **Screen Sleep** on (the default), the TV shows one dim still screen while pictures are converting, and after that **nothing is drawn at all**. The graphics chip puts everything into painting.

- Press any controller button to wake the screen for 15 seconds. The press that wakes the screen does nothing else, so a sleepy press can never stop a conversion by accident.
- The screen wakes by itself when converting ends, or when something needs your attention.
- Switch it off on the phone (Settings), on the TV (Settings, or the Y button on the Convert screen).
- Progress is still shown on the phone page the whole time.

## The phone page

| Tab | What it is for |
| --- | --- |
| **Convert** | Pick the detail level, start and stop, and watch the picture being converted: the original on the left and the painting so far on the right, with progress, strokes per second, leftover error and time left. **Convert them all again** redoes every picture. |
| **Results** | Every finished picture, original and painting side by side. Tap one to look closer. Delete one, or all of them. |
| **Pictures** | Add as many pictures as you like (up to **100,000**, as long as the Xbox has room), or a zip file of them. Each picture is fitted to 128 by 128 pixels on your phone before it is sent, so big photos are fine. Choose between cropping the edges and squashing the whole picture, and how small pictures are enlarged (pixel art can stay crisp). Select and remove pictures, or remove them all. |
| **Settings** | Detail level, Screen Sleep, skipping pictures that are already finished, the random seed, how small pictures are enlarged, and for the curious the number of candidate strokes and tuning rounds. Every setting explains itself, and **Reset** puts the recommended values back. It also shows the version, the address, whether the graphics chip passed its self-test, a button to run the self-test again, and the activity log. |

## The TV screen and controller

| Button | What it does |
| --- | --- |
| **LB / RB** | Switch between Convert, Results, Settings and Phone |
| **Menu** | Go to Settings |
| **View** | Show the Phone address and code, and go back to Convert |
| **A** | Convert: start or stop. Results: open the big view. Settings: change the row |
| **Left / Right** | Convert: change the detail level (only while nothing is converting). Big view: previous / next picture. Settings: change a value |
| **X** | Convert: convert everything again. Results: delete the picture |
| **Y** | Convert: switch Screen Sleep on or off |
| **B** | Back / cancel |
| **D-pad or left stick** | Move around |

## Where things are saved

Everything (pictures, finished cards, settings) is kept in the app's own storage on the Xbox, so it is still there the next time you open the app and after an update. It is deleted if you uninstall the app.

- Pictures: about 48 KB each, so 100,000 pictures take about 4.9 GB. The app keeps about half a gigabyte of the Xbox's storage free: when less than that is left, new pictures are refused with a clear message instead of filling the console up.
- Cards: 3 + 12 bytes per stroke, so even 6,000 strokes is only about 72 KB per picture.
- The app keeps the pictures you added to the older training version of this app. Brains and snapshots that the older version saved are left alone and are not used by this version.

## Good to know (honest limits)

- **The graphics chip is required.** Before the app trusts the chip it runs a self-test of five checks (every program gives the right answer, the stroke scores match the processor's, the same input gives the same dials twice, and so on). If the chip is missing or fails the test, the Convert screen says so and nothing is converted. There is no slower processor path.
- **Pictures are painted in batches.** Several pictures (up to 32, as many as fit in about 512 MB of chip memory) are painted at the same time, because that is much faster than one by one. The cards of a batch are saved when the whole batch is finished. If converting stops in the middle of a batch (you press Stop, the app is put to sleep, the chip is reset), that batch is not kept and its pictures are converted again next time; every batch that was finished stays. The Convert screen shows which batch is running.
- **If the chip is reset by the system** (it can happen when the Xbox sleeps or a driver resets), the app rebuilds the chip and carries on with the pictures that are left.
- **If you switch to another app**, the Xbox pauses this one. The app stops converting cleanly and carries on when you come back.
- **The numbers in the automated tests are not Xbox speed.** The tests here run the chip programs on a software stand-in, so they prove the answers are right, not how fast the real console is. The real speed is what **Speed** shows on your Xbox.
- **Why not a blurred aim?** The first two flights could aim at blurred copies of the picture. Tests on real photos showed this left clearly more error in the end, so every flight aims at the sharp picture.
- **Candidates and tuning rounds** are set to 64 and 3. Raising the candidates to 128 gave roughly 7 % less leftover error in the same tests at a higher cost per stroke. If the real speed on your Xbox leaves room, this can be raised in a later version (or here under Settings).
- The phone page has no password. Anyone on the same Wi-Fi who knows the address can open it, so use it on a network you trust.
- Very large collections (thousands of pictures) take a while to send. Keep the phone screen on until the upload says it is done.
- Every stored picture is checked when the app starts, so a very big set makes the start slower. The loading screen says so.
- The Xbox gives apps more memory and graphics power when it treats them as games. In Developer Mode, open the Device Portal, go to Preference Settings and turn on "Treat UWP apps as games by default", then start the app again.
- For long runs the app asks the system not to dim the display or put the console to sleep while pictures are converting. If your console still turns itself off, open the console's power settings and set "Turn off after" to "Don't turn off automatically".

## If something goes wrong

- **The page does not open:** the phone and the Xbox must be on the same Wi-Fi, and the app must be open on the TV. The page tries again by itself.
- **The page says it cannot reach the Xbox:** wake the Xbox, open the app, wait a few seconds.
- **The Convert screen says the graphics chip is not ready:** open Settings and run the self-test again. The activity log (Settings, at the bottom of the phone page) shows each of the five checks.
- **The TV stays dim:** that is Screen Sleep. Press any button.
- **Activity log:** the phone page's Settings tab has a button that downloads the activity log (including any crash notes). Send it to whoever helps you.

## For developers

- Built by GitHub Actions (`.github/workflows/build-xbox.yml`, windows-2022, Visual Studio 2022, C++20, UWP / Xbox). The workflow copies the files below into `xbox-uwp/src`, compiles the 11 chip programs of `Strokes.hlsl` with `fxc`, bakes a safety-net copy into `ShaderBlobs.h`, packs them into the app, and produces a signed `.msix`.
- `StrokeCodec.*` is the dial table (the card format, the processor painter and scorer). `GpuCore.*` is the chip plumbing (D3D12 compute, a dedicated compute queue, hang detection). `Converter.*` is the three flights and the self-test. `StrokeStore.*` keeps the finished cards (one small file per card, written atomically, found again after a restart by a key made from the picture and every setting that changes the result). `Dataset.*` keeps the pictures. `AppCore.*` ties it together and is the only thing the web page and the TV screen talk to.
- `HttpServer.*`, `WebApi.*`, `WebHost.*` are the phone page's server. `WebUiAssets.h` is the phone page as a byte array (generated from `web/` by `web/build_web.py`). `TvUi.*` decides what the TV shows (and the Screen Sleep rules); `GamepadUI.*` and `Main.cpp` draw it and read the controller. `WebBridge.*` holds the few Windows-only helpers.
- Start-up order (`Main.cpp`): the core and its log hooks, the memory allowance (only logged), the graphics device and TV drawing, the splash and the core start, the phone page, the graphics chip and its self-test, then the TV screens and the main loop. While the screen sleeps the main loop makes no draw calls; it checks twice a second that the graphics device is still there.
- The phone page uses the fonts Bricolage Grotesque and Lora under the SIL Open Font License; see `THIRD_PARTY_FONTS.txt`.
- Everything except the Windows-only files is compiled and tested natively on Linux (address, undefined-behaviour and thread checkers, a real browser against the real server). The chip code (`GpuCore.cpp`, `Converter.cpp` with `Strokes.hlsl`) is run in tests on a software graphics chip that executes the very same shader text and checks it against the processor painter and against the rules of the D3D12 API (buffer states, barriers, ranges). The Windows-only files and the real shader compilation are only done by the GitHub workflow.
