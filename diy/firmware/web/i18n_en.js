// English for the panel. The page is written in Russian; with LANG = "en" every text that reaches
// the screen (text nodes, placeholder/title, toasts, confirm/prompt) goes through tr():
// phrases from EN (longest first, whole Cyrillic words only), then units and numbers.
// Elements with class "notr" (chat messages, console output) are left as they are.
// diy/scripts/embed_web.py inlines this file into index.html at build time.
const EN = [
  ["Дополнительный канал: только чат в панели, в Home Assistant не передаётся. Хранятся последние 16 сообщений с момента загрузки платы.", "Extra channel: panel chat only, not sent to Home Assistant. The last 16 messages since the board started are kept."],
  ["До 4 каналов вдобавок к «Дому» — только для чата в панели: в Home Assistant они не передаются, команды и тревоги работают через «Дом». Ключ — как в приложении (hex или base64), #хэштег или public. Каналы #хэштег и Public не приватные: их читает любой, кто знает название.", "Up to 4 channels in addition to \u201cHome\u201d, for the panel chat only: they are not sent to Home Assistant; commands and alarms go through \u201cHome\u201d. Key as in the app (hex or base64), #hashtag or public. #hashtag and Public channels are not private: anyone who knows the name can read them."],
  ["Введите ключ канала, #хэштег или public", "Enter a channel key, #hashtag or public"],
  ["ключ, #хэштег или public", "key, #hashtag or public"],
  ["имя (необязательно)", "name (optional)"],
  ["ретрансляторы SenkoOS в одной сети обмениваются пакетами по Wi-Fi, а не по LoRa: каждый передаёт услышанное другими в свою сторону", "SenkoOS repeaters on one network exchange packets over Wi-Fi instead of LoRa: each one re-sends what the others heard in its own direction"],
  ["8–32 символа, одинаковый на всех ретрансляторах моста; только запись", "8–32 characters, the same on every bridge repeater; write-only"],
  ["только для ретрансляторов в другой подсети или через VPN (до 4); в своей подсети они находятся сами", "only for repeaters in another subnet or over a VPN (up to 4); in your own subnet they are found automatically"],
  ["одинаковый на всех; по умолчанию 4280", "the same on all; default 4280"],
  ["Мост LAN между ретрансляторами", "LAN bridge between repeaters"], ["Секрет моста", "Bridge secret"], ["Порт UDP", "UDP port"],
  ["Добавить адрес", "Add address"], ["Удалить адрес", "Remove address"], ["включён, других узлов пока нет", "on, no other nodes yet"],
  ["не задан секрет", "no secret set"], ["секрет не задан", "no secret set"], ["запускается…", "starting…"],
  ["кадр, прошедший столько узлов-посредников, отбрасывается (1–8, по умолчанию 2)", "a frame that passed this many relaying nodes is dropped (1–8, default 2)"],
  ["Предел переходов", "Hop limit"], ["сверх лимита", "over the rate limit"], ["по переходам", "by hop limit"], ["повторов", "replays"],
  ["никого не слышно", "nobody heard"], ["мост LAN", "LAN bridge"], ["из LAN", "from LAN"], ["в LAN", "to LAN"], ["чужих", "rejected"], ["адреса", "addresses"], ["Мост", "Bridge"],
  ["Дополнительные каналы", "Extra channels"],
  ["Добавить канал", "Add channel"],
  ["удалить канал", "delete channel"],
  ["Удалить канал", "Delete channel"],
  ["с платы?", "from the board?"],
  ["новые сообщения", "new messages"],
  ["Канал не задан", "Channel is not set"],
  // ---- shell, login, header
  ["Лаборатория ретранслятора", "Repeater lab"], ["лаборатория ретранслятора", "repeater lab"], ["лаборатория", "lab"],
  ["Доступ только для администратора", "Administrator access only"], ["Войти", "Log in"], ["выйти", "log out"],
  ["Неверный пароль", "Wrong password"], ["Слишком много попыток, подождите", "Too many attempts, wait"],
  ["на связи", "online"], ["нет связи", "offline"], ["◐ авто", "◐ auto"], ["☀ светлая", "☀ light"], ["☾ тёмная", "☾ dark"],
  ["Тема: авто / светлая / тёмная", "Theme: auto / light / dark"], ["Ретранслятор", "Repeater"], ["ретранслятор", "repeater"],
  ["релиз", "release"], ["бета", "beta"],
  ["Сборка опубликованного выпуска", "Build of a published release"], ["Предварительный выпуск: для проверки", "Pre-release: for testing"],
  ["Сборка из ветки dev или локальная: не выпуск", "Build from the dev branch or a local one: not a release"],
  ["Обзор", "Overview"], ["Пакеты", "Packets"], ["Каналы", "Channels"], ["Чат", "Chat"], ["Сеть", "Network"], ["Карта", "Map"],
  ["Настройки", "Settings"], ["Консоль", "Console"], ["Обслуживание", "Maintenance"],
  ["Скопировано", "Copied"], ["Готово", "Done"], ["Введите значение", "Enter a value"], ["Ошибка", "Error"], ["Загрузка…", "Loading…"],
  // ---- overview
  ["аптайм", "uptime"], ["старт: питание", "start: power on"], ["старт: кнопка RESET", "start: RESET button"],
  ["старт: перезагрузка/OTA", "start: reboot/OTA"], ["старт после сбоя", "start after a crash"], ["старт: сторож (зависание)", "start: watchdog (hang)"],
  ["старт: из сна", "start: from sleep"], ["старт: просадка питания", "start: brownout"], ["старт: другое", "start: other"],
  ["радио", "radio"], ["шум эфира", "noise floor"], ["Шум эфира, dBm", "Noise floor, dBm"], ["собираю…", "collecting…"],
  ["последний пакет", "last packet"], ["пока ничего не принято", "nothing received yet"], ["прямой", "direct"], ["хоп.", "hop(s)"],
  ["эфир за сутки", "airtime, 24 h"], ["часы", "clock"], ["не задано", "not set"], ["пересылка", "forwarding"],
  ["объявление каждые", "advert every"], ["по сети каждые", "network-wide every"], ["Объявить", "Advertise"],
  ["без ретрансляции — слышат только соседи; с ретрансляцией — вся сеть", "without forwarding — only neighbours hear it; with forwarding — the whole network"],
  ["без ретрансляции", "zero-hop"], ["с ретрансляцией", "flood"],
  ["advert без ретрансляции", "advert zero-hop"], ["advert с ретрансляцией", "advert flood"],
  ["датчик", "sensor"], ["температура", "temperature"], ["влажность", "humidity"], ["давление", "pressure"], ["высота", "altitude"],
  ["напряжение", "voltage"], ["ток", "current"], ["мощность", "power"], ["освещённость", "illuminance"], ["расстояние", "distance"],
  ["частота", "frequency"], ["процент", "percent"], ["концентрация", "concentration"], ["аналоговый вход", "analog input"], ["значение", "value"],
  ["За последний час", "Last hour"], ["Пакеты в минуту", "Packets per minute"], ["принято", "received"], ["передано", "sent"],
  ["Принято", "Received"], ["Передано", "Sent"], ["среднее", "average"], ["мин–макс", "min–max"],
  ["История хранится на репитере с момента загрузки: час по минутам и сутки по 10 минут (вкладка «Пакеты»).",
   "History is kept on the repeater since boot: an hour by minute and a day by 10 minutes (Packets tab)."],
  ["свободно", "free"], ["минимум", "minimum"], ["блок", "block"], ["мин", "min"],
  // ---- packets
  ["Время в эфире", "Airtime"], ["С загрузки", "Since boot"], ["Последние 5 минут", "Last 5 minutes"], ["Последние 24 часа", "Last 24 hours"],
  ["За сутки", "Last 24 hours"], ["Пакеты за 10 минут", "Packets per 10 minutes"], ["Эфир за 10 минут, %", "Airtime per 10 minutes, %"],
  ["Пакеты с загрузки", "Packets since boot"], ["По типам с загрузки", "By type since boot"], ["за", "over"],
  ["ошибок передачи", "TX errors"], ["заблокировано", "blocked"], ["Живой журнал", "Live log"], ["с открытия", "since opened"],
  ["пауза", "pause"], ["продолжить", "resume"], ["сбросить статистику", "reset statistics"],
  ["Новые сверху. RX — принято, TX — передано (свои объявления и пересылки). Клик по первому хопу сообщения канала блокирует входной репитер, по следующему — маршрут.",
   "Newest first. RX — received, TX — sent (own adverts and forwards). Click the first hop of a channel message to block the entry repeater, a later hop — the route."],
  ["Время", "Time"], ["Тип", "Type"], ["Маршрут", "Route"], ["Хопы", "Hops"], ["Длина", "Length"], ["Канал", "Channel"], ["Путь", "Path"],
  ["канальных", "channel"], ["объявлений", "adverts"], ["личных", "direct"], ["прочих", "other"],
  ["запрос", "request"], ["ответ", "response"], ["личное сообщение", "direct message"], ["подтверждение", "ACK"], ["объявление", "advert"],
  ["сообщение канала", "channel message"], ["данные канала", "channel data"], ["анонимный запрос", "anonymous request"], ["путь", "path"],
  ["трассировка", "trace"], ["служебный", "control"],
  // ---- channels
  ["Хэш", "Hash"], ["Сообщений", "Messages"], ["Когда", "When"], ["Блокировать", "Block"], ["хэш (4F) или #хэштег", "hash (4F) or #hashtag"],
  ["Публичный", "Public"], ["пересылать публичный канал", "forward the public channel"], ["хэшей", "hashes"],
  ["Входные репитеры", "Entry repeaters"], ["Узел", "Node"],
  ["Через какой репитер сообщение канала попало в сеть (первый хоп пути). Блокировка входного репитера отсекает его сообщения каналов здесь; личные сообщения, ACK и объявления проходят. Маршрут «45>A1» ищется в любом месте пути; если второй — этот репитер, блокируется то, что 45 передаёт нам.",
   "Which repeater brought a channel message into the network (first hop of the path). Blocking an entry repeater drops its channel messages here; direct messages, ACKs and adverts pass. A route \"45>A1\" is matched anywhere in the path; if the second one is this repeater, what 45 sends to us is blocked."],
  ["репитер (A0 или A07E) или маршрут (45>A1)", "repeater (A0 or A07E) or route (45>A1)"],
  ["заблокировать входной репитер", "block entry repeater"], ["Заблокировать входной репитер", "Block entry repeater"],
  ["заблокировать маршрут", "block route"], ["Заблокировать маршрут", "Block route"], ["снять блокировку", "unblock"], ["снять", "unblock"],
  ["Сообщения каналов через него перестанут пересылаться.", "Channel messages through it will no longer be forwarded."],
  ["Сбросить статистику каналов и входных репитеров?", "Reset channel and entry repeater statistics?"],
  ["канал", "channel"], ["маршрут", "route"], ["репитер", "repeater"],
  // ---- chat
  ["Канал «Дом» (чат с Home Assistant)", "Channel \"Home\" (chat with Home Assistant)"], ["Канал «Дом» не задан", "Channel \"Home\" is not set"],
  ["Канал «Дом»", "Channel \"Home\""], ["Канал «", "Channel \""], ["»", "\""], ["Дом", "Home"],
  ["Сообщения приватного канала MeshCore: то же, что в приложении, и ответы Home Assistant. Хранятся последние 30 с момента загрузки платы.",
   "Messages of the private MeshCore channel: the same as in the app, plus Home Assistant replies. The last 30 since boot are kept."],
  ["сообщение в канал", "message to the channel"], ["Отправить", "Send"],
  ["Длинный текст плата делит на части [1/n] и шлёт с паузой 12 с. Сообщение уходит от имени ретранслятора.",
   "The board splits long text into [1/n] parts sent 12 s apart. Messages go out as the repeater."],
  ["отправляется ещё", "still sending"], ["ч.", "parts"],
  ["Канал не задан: «Настройки → Канал «Дом»» — ключ канала из приложения.", "Channel not set: Settings → Channel \"Home\" — the channel key from the app."],
  ["Пока тихо. Сообщения появятся здесь.", "Quiet so far. Messages will show up here."],
  ["Пустое сообщение", "Empty message"], ["Ещё отправляется предыдущее длинное сообщение", "The previous long message is still being sent"],
  // ---- network
  ["Пути в репитер", "Paths into the repeater"], ["узлов", "nodes"], ["напрямую", "direct"], ["забыто", "forgotten"],
  ["Показывать до", "Show up to"], ["хопов", "hops"], ["из", "of"], ["все пути", "all paths"], ["активные 5 мин", "active 5 min"],
  ["активные 15 мин", "active 15 min"], ["активные 1 ч", "active 1 h"], ["Скрыть пути, по которым давно не было пакетов", "Hide paths without recent packets"],
  ["сбросить", "reset"], ["Сбросить накопленные пути?", "Reset the collected paths?"],
  ["Слева — этот репитер, следующий столбец — соседи, от которых пакеты пришли напрямую (с SNR), дальше — кто передал им. Толщина и порядок — по числу пакетов (×N). Круг — репитер, квадрат — источник объявления, ореол — слышали за последние 20 с. Яркость линии — давность пакетов по этому пути (бегущий пунктир — меньше минуты назад, бледная — больше часа). Клик по узлу выделяет его путь, колесо мыши или два пальца — масштаб, перетаскивание — сдвиг.",
   "Left — this repeater, next column — neighbours we heard packets from directly (with SNR), further — who passed packets to them. Width and order — by packet count (×N). Circle — repeater, square — advert source, halo — heard in the last 20 s. Line brightness — how recent the packets on this path are (running dashes — under a minute, faint — over an hour). Click a node to highlight its path; mouse wheel or two fingers — zoom, drag — pan."],
  ["Приблизить", "Zoom in"], ["Отдалить", "Zoom out"], ["вписать", "fit"], ["Показать весь граф", "Show the whole graph"], ["Исходный масштаб", "Original scale"],
  ["Путей пока нет: ждём flood-пакетов с хопами.", "No paths yet: waiting for flood packets with hops."],
  ["За выбранное время пакетов не было: выберите «все пути».", "No packets in the selected time: choose \"all paths\"."],
  ["цвет ветки = сосед, через которого пришёл путь", "branch colour = the neighbour the path came through"], ["мы (эхо)", "us (echo)"], ["мы", "us"],
  ["пакетов", "packets"], ["последний", "last"],
  ["Соседи", "Neighbours"], ["Сканировать", "Scan"], ["очистить", "clear"], ["Очистить список соседей?", "Clear the neighbour list?"],
  ["Сканирование просит все узлы в зоне слышимости ответить (не чаще раза в 30 с).", "A scan asks every node in range to answer (at most once per 30 s)."],
  ["Идёт сканирование", "Scanning"], ["Запрос отправлен, ответы придут в течение минуты", "Request sent, answers arrive within a minute"],
  ["Имя", "Name"], ["Наш SNR", "Our SNR"], ["Их SNR", "Their SNR"], ["Откуда", "Source"],
  ["объявление, напрямую, скан", "advert, direct, scan"], ["объявление, напрямую", "advert, direct"], ["скан", "scan"],
  ["Пока никого. Нажмите «Сканировать».", "Nobody yet. Press \"Scan\"."],
  ["чат", "chat"], ["комната", "room"], ["следить за узлом", "watch this node"], ["добавить в трассировку", "add to the trace"],
  ["не пересылает трассировку: телефон без режима повторителя или датчик", "does not forward a trace: a phone without repeat mode, or a sensor"],
  ["Трасса через него, скорее всего, не вернётся. Отправить всё равно?", "A trace through it will most likely not come back. Send anyway?"],
  ["Наблюдение за узлами", "Watched nodes"],
  ["Если узел из списка не слышно дольше заданного времени (ни напрямую, ни в путях пакетов), в канал «Дом» и в Home Assistant уходит тревога, а когда он снова слышен — «снова на связи». Глаз 👁 в таблице соседей добавляет узел сюда.",
   "If a node from the list is not heard (directly or in packet paths) for longer than its limit, an alarm goes to the channel \"Home\" and to Home Assistant, and \"back online\" when it is heard again. The 👁 in the neighbour table adds a node here."],
  ["Порог", "Limit"], ["Не слышно", "Not heard for"], ["Состояние", "State"], ["⚠ молчит", "⚠ silent"], ["убрать", "remove"],
  ["ID, например 4869", "ID, e.g. 4869"], ["Следить", "Watch"],
  ["Список пуст. Добавьте ID узла ниже или глазом 👁 в таблице соседей.", "The list is empty. Add a node ID below or with 👁 in the neighbour table."],
  ["ID: 2–8 шестнадцатеричных символов, например 4869", "ID: 2–8 hex characters, e.g. 4869"],
  ["Трассировка туда и обратно", "Round-trip trace"], ["хопы через запятую: 6A,DE или 1A2B3C4D", "hops, comma separated: 6A,DE or 1A2B3C4D"],
  ["Пакет проходит хопы по очереди и возвращается тем же путём; каждый хоп записывает SNR, с которым принял. ID — первые 1, 2 или 4 байта ключа узла. «+» в таблице соседей добавляет узел.",
   "The packet passes the hops in turn and comes back the same way; every hop records the SNR it received with. ID — the first 1, 2 or 4 bytes of the node key. \"+\" in the neighbour table adds a node."],
  ["Трассировок ещё не было.", "No traces yet."], ["Трассировка отправлена", "Trace sent"], ["Ждём ответа…", "Waiting for the answer…"],
  ["Вернулась за", "Came back in"], ["мс", "ms"], ["Не вернулась за 30 с", "Did not come back within 30 s"],
  ["SNR у каждого хопа — с каким уровнем он принял пакет от предыдущего.", "SNR at each hop — how well it received the packet from the previous one."],
  ["Трасса идёт только через узлы, которые пересылают пакеты: репитеры и room server. Телефон пересылает, только если в нём включён режим повторителя.",
   "A trace goes only through nodes that forward packets: repeaters and room servers. A phone forwards only with its repeat mode on."],
  ["Похоже, причина в узле", "Probably caused by node"],
  ["Ещё причины: узел вне зоны или выключен, у него выключена пересылка, пакет потерялся в эфире — повторите попытку.",
   "Other reasons: the node is out of range or off, its forwarding is disabled, or the packet was lost — try again."],
  // ---- map
  ["На карте:", "On the map:"], ["узл. с координатами из", "nodes with coordinates out of"], ["показать всех", "show all"],
  ["координаты этого репитера не заданы (Настройки → Узел)", "this repeater's coordinates are not set (Settings → Node)"],
  ["Линии — соседи напрямую, цвет по SNR; голубые — репитеры через хопы, фиолетовые — остальные узлы.", "Lines — direct neighbours, colour by SNR; blue — repeaters via hops, purple — other nodes."],
  ["Подложка — OpenStreetMap; карта и библиотека Leaflet загружаются браузером из интернета только на этой вкладке. Без интернета остальные вкладки работают как обычно.",
   "Base map — OpenStreetMap; the map and the Leaflet library are loaded by the browser from the internet on this tab only. Without internet the other tabs work as usual."],
  ["Карта недоступна", "The map is unavailable"], ["нет доступа к интернету", "no internet access"], ["этот репитер", "this repeater"],
  ["участники OpenStreetMap", "OpenStreetMap contributors"],
  // ---- settings
  ["Каждое поле — команда CLI, выполняется сразу. Изменения радио применяются после перезагрузки.", "Every field is a CLI command, applied at once. Radio changes apply after a reboot."],
  ["Устройство", "Device"], ["Плата", "Board"], ["Прошивка", "Firmware"], ["Основа", "Base"], ["Публичный ключ", "Public key"], ["копировать", "copy"],
  ["Память", "Memory"], ["Сохранить", "Save"], ["Широта", "Latitude"], ["Долгота", "Longitude"], ["Владелец", "Owner"],
  ["Пароль администратора", "Admin password"], ["Гостевой пароль", "Guest password"], ["не меняется, пока пусто", "unchanged while empty"],
  ["только запись; это же пароль входа в панель, если не задан отдельный", "write-only; also the panel login unless a separate one is set"],
  ["только запись; пусто — гостевой вход выключен", "write-only; empty — guest login off"], ["только запись", "write-only"],
  ["Положение", "Position"], ["вставьте координаты или ссылку Google / Яндекс Карт", "paste coordinates or a Google / Yandex Maps link"], ["где я", "where am I"],
  ["округлить до ~100 м", "round to ~100 m"], ["Сохранить положение", "Save position"], ["сейчас", "now"], ["не сохранено", "not saved"], ["сохранено", "saved"],
  ["Нажмите на карту, вставьте координаты или ссылку. Координаты узел рассылает в объявлениях всей сети.", "Tap the map, paste coordinates or a link. The node sends its coordinates in adverts to the whole network."],
  ["«Где я» работает только при входе по HTTPS-адресу (например, через rep.…).", "\"Where am I\" works only over an HTTPS address (e.g. through rep.…)."],
  ["Карта недоступна (нет интернета у браузера). Вставьте координаты в поле выше.", "The map is unavailable (no internet in the browser). Paste the coordinates above."],
  ["Координаты вне диапазона", "Coordinates out of range"], ["Браузер не дал положение", "The browser did not give a position"],
  ["Радио", "Radio"], ["Частота / BW / SF / CR", "Frequency / BW / SF / CR"], ["применится после перезагрузки", "applies after a reboot"],
  ["МГц", "MHz"], ["BW кГц", "BW kHz"], ["Мощность TX, dBm", "TX power, dBm"], ["мощность чипа SX1262, не больше 22; на антенне больше за счёт усилителя платы: SenkoPhone (E22) до ~30 dBm, Heltec V4 до ~28 dBm, у Heltec V3 усилителя нет", "SX1262 chip power, 22 at most; the board amplifier adds to it at the antenna: SenkoPhone (E22) up to ~30 dBm, Heltec V4 up to ~28 dBm, Heltec V3 has no amplifier"],
  ["Усиленный приём", "Boosted RX gain"], ["вкл", "on"], ["выкл", "off"],
  ["Пересылка", "Forwarding"], ["Пересылать пакеты", "Forward packets"], ["Объявление, мин", "Advert, min"], ["0 — выкл, иначе 60–240", "0 — off, otherwise 60–240"],
  ["Объявление по сети, ч", "Flood advert, h"], ["Flood max, хопов", "Flood max, hops"], ["Flood max без региона", "Flood max without region"],
  ["Flood max для объявлений", "Flood max for adverts"], ["0 — объявления не пересылаются совсем", "0 — adverts are not forwarded at all"],
  ["Поиск петель", "Loop detection"], ["минимальный", "minimal"], ["умеренный", "moderate"], ["строгий", "strict"],
  ["Размер хэша пути", "Path hash size"], ["0 · 1 байт", "0 · 1 byte"], ["1 · 2 байта", "1 · 2 bytes"], ["2 · 3 байта", "2 · 3 bytes"],
  ["Тайминги и прочее", "Timing and more"], ["доля эфира на TX относительно RX (1.0 — поровну)", "share of airtime for TX relative to RX (1.0 — equal)"],
  ["Порог помех", "Interference threshold"], ["0 — выкл", "0 — off"], ["CAD перед передачей", "CAD before TX"], ["AGC reset, с", "AGC reset, s"],
  ["Множитель АЦП", "ADC multiplier"], ["0 — по умолчанию платы (батареи на этой плате нет)", "0 — board default (this board has no battery)"],
  ["Сеть (SSID)", "Network (SSID)"], ["после смены сети нажмите «Подключить» ниже", "after changing the network press \"Reconnect\" below"],
  ["Пароль Wi-Fi", "Wi-Fi password"], ["подключить заново", "reconnect"],
  ["Панель станет недоступна, если сеть или пароль неверны: тогда настройте Wi-Fi по USB.", "The panel becomes unreachable if the network or password is wrong: then set Wi-Fi over USB."],
  ["Переподключить Wi-Fi? Панель на время пропадёт.", "Reconnect Wi-Fi? The panel will be gone for a while."],
  ["Ключ канала", "Channel key"], ["ключ приватного канала из приложения MeshCore (hex или base64); только запись", "the private channel key from the MeshCore app (hex or base64); write-only"],
  ["Название", "Label"], ["только для панели", "panel only"], ["Отправить в канал", "Send to the channel"],
  ["проверка: сообщение уйдёт в канал от имени ретранслятора", "test: the message goes to the channel as the repeater"],
  ["включён", "on"], ["выключен — вставьте ключ канала", "off — paste the channel key"], ["отключить", "switch off"],
  ["Отключить канал «Дом»? Ключ будет стёрт с платы.", "Switch off the channel \"Home\"? The key will be erased from the board."],
  ["Отправлять в HA", "Send to HA"], ["датчики появятся в HA сами (MQTT discovery)", "sensors appear in HA by themselves (MQTT discovery)"],
  ["Брокер", "Broker"], ["адрес Home Assistant с Mosquitto, например 192.168.1.x", "address of Home Assistant with Mosquitto, e.g. 192.168.1.x"],
  ["Порт", "Port"], ["обычно 1883", "usually 1883"], ["Пользователь", "User"], ["отдельный пользователь брокера для ретранслятора", "a separate broker user for the repeater"],
  ["Пароль", "Password"], ["Интервал, с", "Interval, s"], ["как часто отправлять данные, 30–3600", "how often to send data, 30–3600"],
  ["выключено", "off"], ["не задан брокер", "no broker set"], ["нет Wi-Fi", "no Wi-Fi"], ["подключается…", "connecting…"], ["подключено", "connected"],
  ["Регионы", "Regions"], ["Дерево", "Tree"], ["Домашний регион", "Home region"], ["Задать дерево", "Set the tree"],
  ["грамматика как у CLI: region def …", "same grammar as the CLI: region def …"], ["Применить", "Apply"],
  ["выполняет «region def …», затем «region save»", "runs \"region def …\", then \"region save\""],
  ["Применение", "Apply"], ["Перезагрузить, чтобы применить радио", "Reboot to apply the radio"],
  // ---- console
  ["Готово к экспериментам. Команды выполняются на репитере.", "Ready for experiments. Commands run on the repeater."],
  ["команда CLI, например: get radio", "CLI command, e.g.: get radio"], ["Выполнить", "Run"], ["Повторите", "Repeat"],
  ["(нет ответа)", "(no reply)"],
  // ---- maintenance
  ["Автообновление", "Auto-update"], ["Доступно", "Available"], ["проверить сейчас", "check now"], ["установить", "install"],
  ["Режим", "Mode"], ["только сообщать", "notify only"], ["ставить ночью (03–06 МСК)", "install at night (03–06 MSK)"],
  ["Бета-версии", "Beta versions"], ["не ставить", "do not install"], ["ставить", "install"], ["Адрес манифеста", "Manifest URL"],
  ["Плата принимает только выпуск, подписанный ключом из прошивки, и сверяет SHA-256 файла до переключения. Если новая версия трижды перезагрузится в первые 10 минут, плата вернётся на прежнюю. Проверка — через 5 минут после включения и раз в сутки.",
   "The board accepts only a release signed with the key built into the firmware and checks the file's SHA-256 before switching. If the new version restarts three times within 10 minutes, the board goes back to the previous one. Checked 5 minutes after power-on and daily."],
  ["ожидание", "idle"], ["проверка…", "checking…"], ["загрузка", "downloading"], ["перезагрузка…", "rebooting…"], ["ошибка", "error"],
  ["проверено", "checked"], ["новее нет", "nothing newer"], ["прошлое обновление откатилось", "the last update was rolled back"],
  ["в этой сборке нет ключа подписи — автообновление выключено", "no signing key in this build — auto-update is off"],
  ["не задан адрес манифеста", "no manifest URL"], ["подпись не совпала — выпуск отклонён", "signature does not match — release rejected"],
  ["SHA-256 файла не совпал — не установлено", "file SHA-256 does not match — not installed"], ["нечего устанавливать", "nothing to install"],
  ["идёт ручная загрузка", "a manual upload is running"], ["загрузка прервалась", "download interrupted"], ["размер не совпал с манифестом", "size differs from the manifest"],
  ["Проверяем…", "Checking…"], ["Устанавливаем, плата перезагрузится сама", "Installing, the board will reboot itself"], ["Режим сохранён", "Mode saved"],
  ["Сохранено", "Saved"], ["Адрес сохранён", "URL saved"], ["Установить", "Install"], ["и перезагрузить репитер?", "and reboot the repeater?"],
  ["Пароль веб-входа", "Web login password"], ["Текущий пароль", "Current password"], ["Новый пароль", "New password"], ["не меньше 6 символов", "6 characters at least"],
  ["Сменить", "Change"], ["использовать пароль администратора", "use the admin password"],
  ["После смены пароля все сессии завершаются, нужно войти заново.", "After a password change all sessions end; log in again."],
  ["Для входа используется отдельный пароль веб-панели.", "A separate web panel password is used to log in."],
  ["Сейчас для входа используется пароль администратора репитера.", "The repeater admin password is used to log in now."],
  ["Пароль сменён, войдите заново", "Password changed, log in again"], ["Теперь вход по паролю администратора", "Logging in with the admin password now"],
  ["Текущий пароль веб-входа:", "Current web login password:"], ["Пароли не совпадают", "Passwords do not match"],
  ["Обновление прошивки", "Firmware update"], ["из выпуска (или", "from a release (or"], ["из сборки CI), не", "from a CI build), not"],
  ["Файл проверяется по содержимому, имя не важно. Прошивка пишется во второй слот, ключи и настройки сохраняются, после записи репитер перезагрузится.",
   "The file is checked by its content, the name does not matter. It is written to the second slot, keys and settings stay, the repeater reboots afterwards."],
  ["Загрузка идёт в отдельной задаче: репитер продолжает пересылать пакеты. Не закрывайте страницу и не перезагружайте плату до сообщения «Готово».",
   "The upload runs in its own task: the repeater keeps forwarding packets. Do not close the page or reset the board until \"Done\"."],
  ["Файл", "File"], ["Загрузить", "Upload"], ["Выберите файл .bin", "Choose a .bin file"], ["Записать", "Write"], ["Загрузка…", "Uploading…"],
  ["Готово, репитер перезагружается. Обновите страницу через 20 секунд.", "Done, the repeater is rebooting. Reload the page in 20 seconds."],
  ["Связь прервалась", "Connection lost"], ["Демо: загрузка не выполняется", "Demo: nothing is uploaded"],
  ["Это -merged.bin: его прошивают по USB на адрес 0x0. Для обновления нужен файл <тег>.bin", "This is a -merged.bin: flash it over USB at 0x0. An update needs the <tag>.bin file"],
  ["Это не файл прошивки ESP32", "This is not an ESP32 firmware file"], ["Прошивка для другого чипа (другая плата)", "Firmware for another chip (another board)"], ["батарея", "battery"],
  ["Файл слишком короткий", "File too short"], ["Пустой файл", "Empty file"], ["Токен загрузки неверен или истёк — начните заново", "Upload token wrong or expired — start again"],
  ["Перезагрузка", "Reboot"], ["Перезагрузить репитер", "Reboot the repeater"], ["Перезагрузить репитер?", "Reboot the repeater?"],
  ["Перезагрузка… обновите страницу через 15 секунд", "Rebooting… reload the page in 15 seconds"], ["Мастер настройки", "Setup wizard"],
  // ---- setup wizard
  ["Первая настройка", "First setup"], ["подключено напрямую к плате", "connected directly to the board"],
  ["Новый пароль администратора (6–15 символов) — обязательно: сейчас стоит заводской", "New admin password (6–15 characters) — required: the factory one is set now"],
  ["Новый пароль администратора — оставьте пустым, чтобы не менять", "New admin password — leave empty to keep it"],
  ["Новый пароль администратора (6–15 символов)", "New admin password (6–15 characters)"], ["пароль", "password"], ["ещё раз", "again"],
  ["Имя ретранслятора в сети", "Repeater name in the network"], ["например, Dacha Rep", "e.g. Dacha Rep"], ["Домашний Wi-Fi (2,4 ГГц)", "Home Wi-Fi (2.4 GHz)"],
  ["выберите сеть…", "choose a network…"], ["ищем сети…", "looking for networks…"], ["сетей не найдено", "no networks found"], ["поиск недоступен", "scan unavailable"],
  ["открытая", "open"], ["обновить", "refresh"], ["имя сети вручную", "network name by hand"], ["пароль Wi-Fi", "Wi-Fi password"],
  ["Радио — менять, только если ваша сеть на другом профиле", "Radio — change only if your network uses another profile"],
  ["После изменения радио плата перезагрузится.", "After a radio change the board reboots."], ["отмена", "cancel"],
  ["Задайте новый пароль администратора.", "Set a new admin password."], ["Пароль: от 6 до 15 символов.", "Password: 6 to 15 characters."],
  ["Пароли не совпадают.", "Passwords do not match."], ["Нужен свой пароль, не заводской.", "Use your own password, not the factory one."],
  ["Сохранено.", "Saved."], ["Плата подключается к сети", "The board is connecting to"], ["Точка настройки выключится, телефон вернётся в свою сеть.", "The setup access point turns off, the phone returns to its network."],
  ["Адрес панели появится на экране платы через 10–20 секунд — откройте", "The panel address appears on the board screen in 10–20 seconds — open"],
  ["из домашней сети.", "from the home network."], ["Wi-Fi не менялся.", "Wi-Fi unchanged."], ["Радио изменено — плата перезагрузится.", "Radio changed — the board will reboot."],
  ["Вход в панель — новым паролем администратора.", "Log in to the panel with the new admin password."],
  ["Положение ретранслятора задайте потом в панели: «Настройки → Узел», там есть карта.", "Set the repeater position later in the panel: Settings → Node, there is a map."],
  ["Если адрес не появился: тройное нажатие кнопки на плате снова откроет точку настройки.", "If no address appears: a triple click of the board button opens the setup access point again."],
  ["Задайте новый пароль администратора", "Set a new admin password"], ["Пароль: от 6 до 15 символов", "Password: 6 to 15 characters"],
  ["Нужен свой пароль, не заводской", "Use your own password, not the factory one"], ["Имя: не длиннее 31 символа", "Name: 31 characters at most"],
  ["Wi-Fi: имя до 32, пароль до 64 символов", "Wi-Fi: name up to 32, password up to 64 characters"],
  ["Координаты: широта от -90 до 90, долгота от -180 до 180", "Coordinates: latitude -90..90, longitude -180..180"],
  ["Радио: проверьте частоту, BW, SF (5–12), CR (5–8)", "Radio: check frequency, BW, SF (5–12), CR (5–8)"], ["Настройки уже применяются", "Settings are being applied already"],
  ["Сохранить", "Save"],
  // ---- units and small words (after the phrases)
  ["Узлы, приславшие координаты в объявлениях. Линии — соседи, которых мы слышим напрямую (цвет по SNR).", "Nodes that sent coordinates in adverts. Lines — neighbours we hear directly (colour by SNR)."],
  ["объявление пересылается по всей сети", "the advert is forwarded through the whole network"], ["объявление слышат только соседи", "the advert is heard by neighbours only"],
  ["отправлено", "sent"], ["тег", "tag"], ["ч", "h"],
  ["назад", "ago"], ["КБ", "KB"], ["кГц", "kHz"], ["гПа", "hPa"], ["км/ч", "km/h"], ["байт", "bytes"],
];

// numbers with units: "7 ч 0 мин", "12 с назад", graph "15м"
const EN_UNITS = [
  [/(\d) д(?![А-Яа-яЁё])/g, "$1 d"], [/(\d) ч(?![А-Яа-яЁё])/g, "$1 h"], [/(\d) с(?![А-Яа-яЁё])/g, "$1 s"],
  [/(\d)д(?![А-Яа-яЁё])/g, "$1d"], [/(\d)ч(?![А-Яа-яЁё])/g, "$1h"], [/(\d)м(?![А-Яа-яЁё])/g, "$1m"], [/(\d)с(?![А-Яа-яЁё])/g, "$1s"],
  [/(\d) м(?![А-Яа-яЁё])/g, "$1 m"], [/(\d) В(?![А-Яа-яЁё])/g, "$1 V"],
];

let LANG = "ru";
try { LANG = localStorage.getItem("lang") || ((navigator.language || "ru").toLowerCase().startsWith("ru") ? "ru" : "en"); } catch (e) {}
let EN_RE = null, EN_MAP = null;
function tr(s) {
  if (LANG !== "en" || !s || !/[А-Яа-яЁё]/.test(s)) return s;
  if (!EN_RE) {
    EN_MAP = new Map(EN);
    const keys = [...EN_MAP.keys()].sort((a, b) => b.length - a.length).map(k => k.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"));
    EN_RE = new RegExp("(?<![А-Яа-яЁё])(" + keys.join("|") + ")(?![А-Яа-яЁё])", "g");
  }
  s = s.replace(EN_RE, m => EN_MAP.get(m));
  for (const [re, r] of EN_UNITS) s = s.replace(re, r);
  return s;
}
function trTree(root) {
  if (LANG !== "en" || !root) return;
  if (root.nodeType === 3) {
    const p = root.parentElement;
    if (p && !p.closest(".notr,script,style")) { const v = tr(root.nodeValue); if (v !== root.nodeValue) root.nodeValue = v; }
    return;
  }
  if (root.nodeType !== 1 || root.closest(".notr,script,style")) return;
  const els = [root, ...root.querySelectorAll("[placeholder],[title]")];
  for (const el of els) for (const a of ["placeholder", "title"]) {
    const v = el.getAttribute && el.getAttribute(a);
    if (v && !el.closest(".notr")) { const t = tr(v); if (t !== v) el.setAttribute(a, t); }
  }
  const w = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
  let n;
  while ((n = w.nextNode())) trTree(n);
}
if (LANG === "en") {
  document.documentElement.lang = "en";
  const _confirm = window.confirm.bind(window), _prompt = window.prompt.bind(window);
  window.confirm = m => _confirm(tr(m));
  window.prompt = (m, d) => _prompt(tr(m), d);
  new MutationObserver(ms => {
    for (const m of ms) {
      if (m.type === "characterData") trTree(m.target);
      else if (m.type === "attributes") trTree(m.target);
      else m.addedNodes.forEach(trTree);
    }
  }).observe(document.documentElement, {childList: true, subtree: true, characterData: true, attributes: true, attributeFilter: ["placeholder", "title"]});
  document.addEventListener("DOMContentLoaded", () => { trTree(document.body); document.title = tr(document.title); });
}
