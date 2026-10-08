/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Minimal STM32G031 device header.
 *
 * GENERATED from STM32CubeCLT's own CMSIS-SVD for this part, by
 * tools/gen-stm32g031-header.py.  Addresses and offsets are therefore ST's,
 * not anyone's recollection.  Regenerate rather than edit.
 *
 * Only the peripherals the supervisor touches are here.  A vendored CMSIS
 * header would be ~10 000 lines of a part this firmware uses a dozen
 * registers of, and SWA-22's whole argument is that this firmware can be read
 * in one sitting.  UR-CON-04's vendoring rules are satisfied the same way: no
 * build-time fetch, and what is checked in is small enough to review.
 */
#ifndef STM32G031_H
#define STM32G031_H

#include <stdint.h>

#define SUP_REG32(addr) (*(volatile uint32_t *)(addr))

/* --- peripheral bases --------------------------------------------------- */
#define FLASH_BASE     0x40022000u
#define GPIOA_BASE     0x50000000u
#define GPIOB_BASE     0x50000400u
#define GPIOC_BASE     0x50000800u
#define IWDG_BASE      0x40003000u
#define RCC_BASE       0x40021000u
#define SPI1_BASE      0x40013000u
#define SPI2_BASE      0x40003800u
#define USART1_BASE    0x40013800u
#define USART2_BASE    0x40004400u

/* --- FLASH ---------------------------------------------------------- */
#define FLASH_ACR        SUP_REG32(FLASH_BASE + 0x0u)

/* --- GPIOA ---------------------------------------------------------- */
#define GPIOA_MODER      SUP_REG32(GPIOA_BASE + 0x0u)
#define GPIOA_OTYPER     SUP_REG32(GPIOA_BASE + 0x4u)
#define GPIOA_OSPEEDR    SUP_REG32(GPIOA_BASE + 0x8u)
#define GPIOA_PUPDR      SUP_REG32(GPIOA_BASE + 0xCu)
#define GPIOA_IDR        SUP_REG32(GPIOA_BASE + 0x10u)
#define GPIOA_ODR        SUP_REG32(GPIOA_BASE + 0x14u)
#define GPIOA_BSRR       SUP_REG32(GPIOA_BASE + 0x18u)
#define GPIOA_LCKR       SUP_REG32(GPIOA_BASE + 0x1Cu)
#define GPIOA_AFRL       SUP_REG32(GPIOA_BASE + 0x20u)
#define GPIOA_AFRH       SUP_REG32(GPIOA_BASE + 0x24u)
#define GPIOA_BRR        SUP_REG32(GPIOA_BASE + 0x28u)

/* --- GPIOB ---------------------------------------------------------- */
#define GPIOB_MODER      SUP_REG32(GPIOB_BASE + 0x0u)
#define GPIOB_OTYPER     SUP_REG32(GPIOB_BASE + 0x4u)
#define GPIOB_OSPEEDR    SUP_REG32(GPIOB_BASE + 0x8u)
#define GPIOB_PUPDR      SUP_REG32(GPIOB_BASE + 0xCu)
#define GPIOB_IDR        SUP_REG32(GPIOB_BASE + 0x10u)
#define GPIOB_ODR        SUP_REG32(GPIOB_BASE + 0x14u)
#define GPIOB_BSRR       SUP_REG32(GPIOB_BASE + 0x18u)
#define GPIOB_LCKR       SUP_REG32(GPIOB_BASE + 0x1Cu)
#define GPIOB_AFRL       SUP_REG32(GPIOB_BASE + 0x20u)
#define GPIOB_AFRH       SUP_REG32(GPIOB_BASE + 0x24u)
#define GPIOB_BRR        SUP_REG32(GPIOB_BASE + 0x28u)

/* --- IWDG ---------------------------------------------------------- */
#define IWDG_KR          SUP_REG32(IWDG_BASE + 0x0u)
#define IWDG_PR          SUP_REG32(IWDG_BASE + 0x4u)
#define IWDG_RLR         SUP_REG32(IWDG_BASE + 0x8u)
#define IWDG_SR          SUP_REG32(IWDG_BASE + 0xCu)
#define IWDG_WINR        SUP_REG32(IWDG_BASE + 0x10u)

/* --- RCC ---------------------------------------------------------- */
#define RCC_CR           SUP_REG32(RCC_BASE + 0x0u)
#define RCC_CFGR         SUP_REG32(RCC_BASE + 0x8u)
#define RCC_IOPENR       SUP_REG32(RCC_BASE + 0x34u)
#define RCC_APBENR1      SUP_REG32(RCC_BASE + 0x3Cu)
#define RCC_APBENR2      SUP_REG32(RCC_BASE + 0x40u)

/* --- SPI1 ---------------------------------------------------------- */
#define SPI1_CR1         SUP_REG32(SPI1_BASE + 0x0u)
#define SPI1_CR2         SUP_REG32(SPI1_BASE + 0x4u)
#define SPI1_SR          SUP_REG32(SPI1_BASE + 0x8u)
#define SPI1_DR          SUP_REG32(SPI1_BASE + 0xCu)

/* --- SPI2 ---------------------------------------------------------- */
#define SPI2_CR1         SUP_REG32(SPI2_BASE + 0x0u)
#define SPI2_CR2         SUP_REG32(SPI2_BASE + 0x4u)
#define SPI2_SR          SUP_REG32(SPI2_BASE + 0x8u)
#define SPI2_DR          SUP_REG32(SPI2_BASE + 0xCu)

/* --- USART2 ---------------------------------------------------------- */
#define USART2_CR1       SUP_REG32(USART2_BASE + 0x0u)
#define USART2_CR2       SUP_REG32(USART2_BASE + 0x4u)
#define USART2_CR3       SUP_REG32(USART2_BASE + 0x8u)
#define USART2_BRR       SUP_REG32(USART2_BASE + 0xCu)
#define USART2_ISR       SUP_REG32(USART2_BASE + 0x1Cu)
#define USART2_ICR       SUP_REG32(USART2_BASE + 0x20u)
#define USART2_RDR       SUP_REG32(USART2_BASE + 0x24u)
#define USART2_TDR       SUP_REG32(USART2_BASE + 0x28u)

#endif /* STM32G031_H */
