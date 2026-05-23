# DZFoot 60Hz Refactor — Tests

## Fichiers

| Fichier | Description |
|---------|-------------|
| `test_60hz_physics.cpp` | Tests C++ unitaires (déterminisme, vitesse joueur, physique balle, tick rate). Link avec `libgame`. |
| `test_server_tickrate.py` | Test Python d'intégration : lance `gf_server`, mesure la fréquence réelle des ticks et vérifie la stabilité. |

## Prérequis

- Build GF Server complété (`build/gf_server` existe).
- Pour le test Python : Python 3.8+ (pas de dépendances externes).
- Pour le test C++ : même toolchain que le build principal (CMake, g++11).

## Exécution

### 1. Test C++ (recommandé pour valider physique & déterminisme)

**Le test ne recompile PAS le serveur.** Il est compilé conditionnellement via `BUILD_60HZ_TEST=ON`.

```bash
cd dzfoot-gf-server

# Build uniquement le test (gf_server reste intact)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_60HZ_TEST=ON
cmake --build build --target test_60hz_physics
./build/tests/test_60hz_physics
```

**Tests effectués :**
- `Tick Rate` : `kTimeStepMs ≈ 16.6667`, `physics_steps_per_frame == 1`
- `Match Duration` : simulation 60s se termine sans freeze
- `Ball Physics` : friction/drag ralentissent la balle naturellement
- `Player Sprint` : vitesse max observée entre 6 et 10 m/s (~8 attendu)
- `Determinism` : 2 runs identiques → `GameState` identiques tick par tick

### 2. Test Python (recommandé pour valider le serveur complet)

```bash
cd dzfoot-gf-server/tests
python3 test_server_tickrate.py
```

**Tests effectués :**
- `Tick Rate` : mesure Hz réel du serveur (attendu ~60 Hz, tolérance ±15%)
- `Heartbeat` : vérifie 1 Hz heartbeat Redis
- `Timer Monotonicity` : timer match croît strictement
- `No Errors` : pas de fatal/assert dans les logs

## Interprétation des résultats

| Scénario | Interprétation |
|----------|----------------|
| **Tous PASS** | Refonte 60Hz stable. Peux passer en production. |
| **Determinism FAIL** | Non-déterminisme détecté. Vérifier `random()`, `boostrandom`, `std::sort` non-stable, threads. |
| **Player Sprint FAIL** | Coefficients physiques mal recalibrés. Vérifier `humanoidbase.cpp` (`maxChange`, `maxAccelerationMPS`, `timeStep_ms`). |
| **Ball Physics FAIL** | Drag/friction/bounce trop fort ou trop faible. Vérifier `ball.cpp` (`drag`, `friction`, `linearFriction`). |
| **Tick Rate FAIL** | Serveur ne cadence pas à 60 Hz. Vérifier `GameServer.cpp` (`simTicksPerSec`, `framePeriod`). |
| **Timer non-monotone** | `matchTime_ms` ou `actualTime_ms` recule. Vérifier `BumpActualTime_ms`. |

## Débogage avancé

Si un test échoue, compiler en Debug et activer les logs GF :

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target test_60hz_physics
GF_VERBOSE=1 ./build/tests/test_60hz_physics
```

Pour inspecter les `GameState` tick par tick, ajouter un `std::cout` dans `GameServer::tick()` avant/après `gameEnv_->step()`.
