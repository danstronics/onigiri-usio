#ifndef TEST_PICO_FLASH_H
#define TEST_PICO_FLASH_H
#define PICO_OK 0
int flash_safe_execute(void (*)(void *), void *, unsigned);
#endif
