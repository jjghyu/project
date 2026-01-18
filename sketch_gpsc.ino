/*
  ГИСЧ на ESP32 с гибридным источником энтропии.
  Источник 1: Аналоговый шум АЦП (пин 36, floating).
  Источник 2: Аппаратный ГСЧ esp_random() (физический шум чипа).
  Алгоритм: XOR двух источников -> LSB -> Фон Нейман -> SHA-256 -> USB.
*/

#include "mbedtls/sha256.h"

// КОНФИГУРАЦИЯ 
#define ADC_NOISE_PIN    36      // Пин для аналогового шума (ничего не подключать!)
#define BUFFER_BITS      256     // 32 байта для SHA-256

// ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ
uint8_t entropyBuffer[BUFFER_BITS / 8] = {0};
size_t collectedBits = 0;
bool lastBit = 0;
bool hasLastBit = false;

// 1. ГИБРИДНЫЙ ИСТОЧНИК ЭНТРОПИИ 
bool readHybridNoiseBit() {
  // Источник A: сырое значение АЦП с плавающего пина
  static uint32_t adcSeed = 0;
  
  int adcRaw = analogRead(ADC_NOISE_PIN);
  adcSeed = (adcSeed << 1) | (adcRaw & 0x01);
  
  // Источник B: аппаратный ГСЧ ESP32 (использует физический шум)
  uint32_t hwRandom = esp_random();
  
  // Смешиваем оба источника через XOR
  uint32_t mixed = adcSeed ^ hwRandom;
  
  // Берём младший бит результата
  return (mixed & 0x01);
}

//  2. ФОН НЕЙМАН
bool vonNeumannCorrector(bool currentBit, bool &outputBit) {
  if (!hasLastBit) {
    lastBit = currentBit;
    hasLastBit = true;
    return false;
  }
  hasLastBit = false;
  
  if (lastBit != currentBit) {
    outputBit = currentBit;
    return true;
  }
  return false;
}

// 3. УПАКОВКА БИТОВ В БУФЕР
void packBitIntoBuffer(bool bit) {
  size_t byteIndex = collectedBits / 8;
  size_t bitIndex = collectedBits % 8;
  
  if (bit) {
    entropyBuffer[byteIndex] |= (1 << bitIndex);
  }
  
  collectedBits++;
}

// 4. SHA-256 И ВЫВОД 
void hashAndOutput() {
  if (collectedBits < BUFFER_BITS) return;
  
  uint8_t digest[32];
  mbedtls_sha256_context ctx;
  
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, entropyBuffer, sizeof(entropyBuffer));
  mbedtls_sha256_finish(&ctx, digest);
  mbedtls_sha256_free(&ctx);
  
  // ВЫВОД В HEX (наглядно и гарантированно видно)
  for (int i = 0; i < 32; i++) {
    if (digest[i] < 0x10) Serial.print("0");
    Serial.print(digest[i], HEX);
  }
  Serial.println();
  
  // Сброс буфера
  collectedBits = 0;
  memset(entropyBuffer, 0, sizeof(entropyBuffer));
}

// SETUP И LOOP
void setup() {
  Serial.begin(115200);
  while (!Serial); // Ждём открытия порта (для некоторых плат)
  
  // Настройка АЦП для шума
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  pinMode(ADC_NOISE_PIN, INPUT);
  
  // Прогрев АЦП (делаем несколько холостых чтений)
  for (int i = 0; i < 100; i++) analogRead(ADC_NOISE_PIN);
  
}

void loop() {
  // 1. Получаем бит от гибридного источника
  bool rawBit = readHybridNoiseBit();
  
  // 2. Фильтруем через фон Неймана
  bool correctedBit;
  if (vonNeumannCorrector(rawBit, correctedBit)) {
    // 3. Упаковываем в буфер
    packBitIntoBuffer(correctedBit);
    // 4. Хешируем и выводим при заполнении
    hashAndOutput();
  }
  
  // Небольшая пауза для стабильности АЦП
  delayMicroseconds(50);
}

