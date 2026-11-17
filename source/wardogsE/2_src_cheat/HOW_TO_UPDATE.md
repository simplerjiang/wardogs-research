КАК ОБНОВИТЬ ЧИТ ПОСЛЕ ПАТЧА ИГРЫ (CoreWD) — пошагово
======================================================

Патч игры сдвигает три глобальных адреса в exe. Ниже — как найти новые.

Ш0. ПОДГОТОВКА
   - SDK игры (свежий дампер) для сверки структур
   - драйвер замаплен:  CoreWD\START.bat от админа
     проверка:          tools\drvping.exe
   - игра запущена

Ш1. GWorld
   Ищем RVA, где rd64(base+RVA) даёт указатель на мир. Проверяем СТРОГО:
       world+0x30  -> Level (валидный указатель)
       Level+0xA0  -> Actors data ;  Level+0xA8 -> Actors count (100..400000)
       world+0x1B0 -> GameState (валидный)
       GameState+0x2D0 -> PlayerArray, count 0..4096
   ПРИЗНАК ВЕРНОГО: PlayerArray count = 100.
   Скан-находки с count 132181/207518 — ЛОЖНЫЕ.

Ш2. GObjects
   rd64(base+RVA) -> массив; rd64(массив) -> чанк; элемент = чанк + idx*0x18 + 8.
   Класс объекта: obj+0x10 -> +0x18 -> FName idx.
   Проверка: первые объекты = /Script/CoreUObject, Object, /Script/Engine (6/6).

Ш3. GNames  (самое коварное)
   rd64(base+RVA) -> ТАБЛИЦА БЛОКОВ (не сам блок).
       block  = rd64(таблица + (idx>>16) * 8)
       entry  = block + (idx & 0xFFFF) * 8
       hdr    = uint16 @ entry+8 ;  len = hdr >> 6
       текст  = entry + 0x0C, len байт
   Проверка: idx 0 = "None", idx 2 = "ByteProperty", idx 5 = "IntProperty",
             idx 0x10000 = осмысленное имя (проверка перехода во 2-й блок).

   !!! ГЛАВНАЯ ЛОВУШКА !!!
   Почти все классы и теги лежат по индексам ВЫШЕ 0xFFFF. Если резолвер
   не делает переход (idx>>16), низкие имена работают, а всё важное молча
   отваливается: серые боксы/скелеты (теги фракций) и пустое оружие.
   Имена игроков при этом могут читаться — они на низких индексах.

Ш4. ПРАВКА КОНСТАНТ —  src\wd_esp_full.c
       #define RVA_UWORLD     0xD2AFC08u
       #define RVA_GENGINE    0xD2B2EC0u
       #define GOBJECTS_RVA   0xD1234E0u
       #define GN_RVA         0xD050CC0u
       #define GN_BLOCKS      0x10u
       #define GN_ENTRY_SZ    8u
       #define GN_HEADER      0x08u
       #define GN_DATA        0x0Cu

   ПРОВЕРЕНО ЖИВЬЁМ на билде 15.10.2026 (новый сезон).
   Метод: каждый RVA проверяется цепочкой разыменований, а не одним чтением.
   GNames — по индексам 2/5/0x10000 (ByteProperty/IntProperty/<имя>), это ловит
   и верный блок, и верный блок-офсет (+0x10) одновременно.

Ш5. СВЕРКА СТРУКТУР ПО SDK  —  сдвиги НЕ одинаковые
       UWorld:                 PersistentLevel 0x30 | GameState 0x1B0 | GI 0x228
       AGameStateBase:         PlayerArray 0x2D0
       APlayerState:           PawnPrivate 0x330 | PlayerNamePrivate 0x360
       APawn:                  Controller 0x2E8 | PlayerState 0x2D8
       ACharacter:             Mesh 0x3A0
       USkeletalMeshComponent: AnimInstance 0x958 | SkinnedAsset 0x5B0
       AnimInstance:           HeldItem (TWeakObjectPtr) 0xC54
       ABHItem:                ItemId (FGameplayTag) 0x328
       AWDMoverCharacter:      Vitality 0x748 | Inventory 0x758 | Operator 0x768
                               WeaponBehavior 0x780 | VitalityState 0x804
       UWDVitalityComponent:   MaxHealth 0x134 | health attr 0x140
       AWDPlayerStateSession:  FactionComponent 0x5A0 | SquadComponent 0x5A8
       UWDFactionComponent:    AWDFaction* 0x128
       UWDPlayerInventoryComponent: armor TArray 0x830
   FTransform:  Rotation 0x00 | Translation 0x20 | Scale3D 0x40 | stride 0x60

   Здоровье (НЕ инлайн):
       vital = rd64(pawn+0x748); attr = rd64(vital+0x140); hp = double @ attr+0x30

   ГРАБЛИ, на которые я наступил сам:
   1. OFF_SC_C2W (0x210) и OFF_CAM_CACHE (0x1560+0x10) — ИНЛАЙНОВЫЕ структуры,
      не указатели. Разыменование даёт 0, и это выглядит как «оффсет уехал».
   2. OFF_PS_NAME 0x350 не мёртв — он читается, но заполнен только у локального
      игрока. Проверка на себе проходит, на чужих вылезает "?". Смотреть 0x360.
   3. Сдвиг +0x10 верен для AWDMoverCharacter и AWDPlayerStateSession, но
      UWDPlayerInventoryComponent уехал на +0x18 (якоря: InputMappingContext
      0x5C8->0x5E0, ABHItem* 0x8C8->0x8E0). Не прибавлять 0x10 вслепую.
   4. 0xA18 (CachedComponentSpaceTransforms) на новом билде читается нулём —
      живой массив костей на OFF_MESH_BONES (0x620), 356 трансформов.

Ш6. СБОРКА
       cd CoreWD\src && build_new.bat
       copy WarDogsESP_new.exe ..\WarDogsESP.exe
       запустить START.bat (перезагрузка драйвера не нужна, если замаплен)

Ш7. ДИАГНОСТИКА
   симптом                           причина
   имён нет вообще                   GNames (Ш3)
   имена есть, цвета серые           переход по блокам (idx>>16)
   пустое оружие                     то же
   health всегда 100                 attr-указатель (vital+0x140)
   чит не стартует                   драйвер: tools\drvping.exe
   КРАКОЗЯБРЫ в никах и названиях    вывод текста, НЕ чтение!
                                     чтение проверять в
                                     E:\Project\wardogs\WarDogsESP\dbg.txt:
                                     если там ники верные — править DrawTextC
                                     (MultiByteToWideChar + TextOutW)
   исходник поплыл целиком           кодировка: писать байтами UTF-8 напрямую,
                                     без промежуточного перекодирования

Ш8. ЗАПРЕТЫ
   - не мапить драйвер второй раз за загрузку (0x109)
   - не брать RVA с форума без проверки сканом
   - не менять GN_ENTRY_SZ (здесь 8)
