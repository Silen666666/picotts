# Kniffel Android App

Native Android Kniffel (Yahtzee) App für 1–5 Spieler. Würfeln durch Handy schütteln.

## Build

1. **Android Studio** öffnen → „Open an Existing Project" → diesen Ordner wählen
2. Gradle sync abwarten
3. Gerät/Emulator auswählen → **Run** (▶)

Oder per Kommandozeile (Android SDK + JDK 8+ nötig):
```bash
./gradlew assembleDebug
# APK: app/build/outputs/apk/debug/app-debug.apk
```

## Voraussetzungen

- Android Studio Hedgehog (2023.1) oder neuer
- Android SDK 34
- Kotlin 1.9
- Mindest-Android-Version: 5.0 (API 21)

## Features

- 1–5 Spieler mit eigenen Namen
- **Schütteln** des Handys würfelt (Accelerometer)
- Würfel durch Antippen halten
- Vollständiges Kniffel-Regelwerk inkl. Bonus
- Punktevorschau vor der Wertung
- Kategorie streichen mit Bestätigung
- Ergebnisscreen mit Rangliste
