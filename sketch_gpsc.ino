/*
  ГИСЧ на ESP32 с гибридным источником энтропии.
  Архитектура: RF-шум (через АЦП) + тепловой шум чипа → XOR → фон Нейман → SHA-256
  Выход: криптографически стойкие 256-битные ключи в HEX-формате
*/

#include "mbedtls/sha256.h"  // Криптографическая библиотека для SHA-256

// КОНФИГУРАЦИЯ 
#define ADC_NOISE_PIN    36  // Пин для захвата RF-шума (плавающий, без подключения)
#define BUFFER_BITS      256 // Размер буфера энтропии для SHA-256 (32 байта = 256 бит)

// ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ 
uint8_t entropyBuffer[BUFFER_BITS / 8] = {0};  // Буфер для накопления энтропии
size_t collectedBits = 0;                      // Счётчик собранных бит
bool lastBit = 0;                              // Для корректора фон Неймана
bool hasLastBit = false;                       // Флаг наличия сохранённого бита

// 1. ГИБРИДНЫЙ ИСТОЧНИК ЭНТРОПИИ
bool readHybridNoiseBit() {
  // Источник A: RF-шум через плавающий пин АЦП 
  static uint32_t adcSeed = 0;                  // Сдвиговый регистр для накопления истории
  
  int adcRaw = analogRead(ADC_NOISE_PIN);       // Чтение аналогового шума (12 бит)
  adcSeed = (adcSeed << 1) | (adcRaw & 0x01);   // Обновление истории: сдвиг + новый младший бит
  
  // Источник B: аппаратный ГИСЧ ESP32 (использует тепловой/внутренний шум чипа)
  uint32_t hwRandom = esp_random();             // Встроенный физический генератор
  
  // Криптографическое смешивание: XOR гарантирует стойкость даже при отказе одного источника
  uint32_t mixed = adcSeed ^ hwRandom;
  
  return (mixed & 0x01);  // Берём самый хаотичный младший бит
}

// 2. КОРРЕКТОР ФОН НЕЙМАНА
// Устраняет статистический перекос (превышение 0 над 1 или наоборот)
bool vonNeumannCorrector(bool currentBit, bool &outputBit) {
  if (!hasLastBit) {                   // Первый бит пары - сохраняем
    lastBit = currentBit;
    hasLastBit = true;
    return false;                      // Пара ещё не готова
  }
  hasLastBit = false;                  // Второй бит пары - обрабатываем
  
  if (lastBit != currentBit) {         // Только пары 01 и 10 дают выход
    outputBit = currentBit;            // 01 → 1, 10 → 0
    return true;                       // Получен валидный бит
  }
  return false;                        // Пары 00 и 11 отбрасываются
}

// 3. НАКОПЛЕНИЕ ЭНТРОПИИ
void packBitIntoBuffer(bool bit) {
  size_t byteIndex = collectedBits / 8;  // Номер байта в буфере (0-31)
  size_t bitIndex = collectedBits % 8;   // Позиция бита в байте (0-7)
  
  if (bit) {
    entropyBuffer[byteIndex] |= (1 << bitIndex);  // Установка бита
  }
  
  collectedBits++;  // Увеличиваем счётчик собранных бит
}

// 4. КРИПТОГРАФИЧЕСКОЕ УСИЛЕНИЕ И ВЫВОД
void hashAndOutput() {
  if (collectedBits < BUFFER_BITS) return;  // Ждём заполнения буфера (256 бит)
  
  uint8_t digest[32];                      // Для хранения хеша (256 бит)
  mbedtls_sha256_context ctx;              // Контекст SHA-256
  
  // Вычисление SHA-256: превращает "сырую" энтропию в криптостойкий ключ
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, entropyBuffer, sizeof(entropyBuffer));
  mbedtls_sha256_finish(&ctx, digest);
  mbedtls_sha256_free(&ctx);
  
  // Вывод в HEX-формате (64 символа) - готовый 256-битный криптоключ
  for (int i = 0; i < 32; i++) {
    if (digest[i] < 0x10) Serial.print("0");  // Добавляем ведущий ноль для однозначности
    Serial.print(digest[i], HEX);
  }
  Serial.println();  // Каждый ключ с новой строки
  
  // Сброс для следующего цикла генерации
  collectedBits = 0;
  memset(entropyBuffer, 0, sizeof(entropyBuffer));
}

// ИНИЦИАЛИЗАЦИЯ
void setup() {
  Serial.begin(115200);
  while (!Serial);  // Ожидание подключения монитора порта (доп. задержка для инициализации)
  
  // Настройка АЦП для максимальной чувствительности к шуму
  analogReadResolution(12);            // 12 бит = 4096 значений
  analogSetAttenuation(ADC_11db);      // Максимальный диапазон напряжения
  pinMode(ADC_NOISE_PIN, INPUT);       // Пин в режиме входа
  
  // Прогрев АЦП: 100 холостых чтений для стабилизации физических параметров
  for (int i = 0; i < 100; i++) analogRead(ADC_NOISE_PIN);
}

// ОСНОВНОЙ ЦИКЛ
void loop() {
  // 1. Захват гибридной энтропии (RF-шум + тепловой шум)
  bool rawBit = readHybridNoiseBit();
  
  // 2. Статистическая коррекция (гарантирует равномерное распределение 0 и 1)
  bool correctedBit;
  if (vonNeumannCorrector(rawBit, correctedBit)) {
    // 3. Накопление в буфере
    packBitIntoBuffer(correctedBit);
    // 4. Генерация ключа при заполнении буфера
    hashAndOutput();
  }
  
  delayMicroseconds(50);  // Пауза для устранения корреляции между измерениями АЦП
}
