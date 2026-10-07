#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Bitcrush Testing
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Regenerate supervisor/board/stm32g031.h from ST's own CMSIS-SVD.
#
#   tools/gen-stm32g031-header.py [path/to/STM32G031.svd]
#
# The SVD ships with STM32CubeCLT, so this is not a network fetch (CON-04).
# Run it rather than editing the header: the point of generating it is that no
# address in it is anybody's recollection.
import sys, pathlib, xml.etree.ElementTree as ET

DEFAULT_SVD = ('/opt/St/STM32CubeCLT_1.17.0/STMicroelectronics_CMSIS_SVD/'
               'STM32G031.svd')
OUT = pathlib.Path(__file__).resolve().parent.parent / 'supervisor/board/stm32g031.h'

# Only what the supervisor touches.  Add a register here, regenerate, review.
WANT = {
    'RCC':    ['CR', 'CFGR', 'IOPENR', 'APBENR1', 'APBENR2'],
    'GPIOA':  ['MODER', 'OTYPER', 'OSPEEDR', 'PUPDR', 'IDR', 'ODR', 'BSRR',
               'LCKR', 'AFRL', 'AFRH', 'BRR'],
    'GPIOB':  [], 'GPIOC': [],
    'SPI1':   ['CR1', 'CR2', 'SR', 'DR'],
    'USART1': ['CR1', 'CR2', 'CR3', 'BRR', 'ISR', 'ICR', 'RDR', 'TDR'],
    'USART2': [],
    'IWDG':   ['KR', 'PR', 'RLR', 'SR', 'WINR'],
    'FLASH':  ['ACR'],
}

HEADER = '''/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
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
 * registers of, and AD-22's whole argument is that this firmware can be read
 * in one sitting.  CON-04's vendoring rules are satisfied the same way: no
 * build-time fetch, and what is checked in is small enough to review.
 */
#ifndef STM32G031_H
#define STM32G031_H

#include <stdint.h>

#define SUP_REG32(addr) (*(volatile uint32_t *)(addr))
'''


def main() -> int:
    svd = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else DEFAULT_SVD)
    if not svd.exists():
        print(f'no SVD at {svd}; pass its path', file=sys.stderr)
        return 1
    root = ET.parse(svd).getroot()
    if root.findtext('name') != 'STM32G031':
        print(f"{svd} is {root.findtext('name')}, not STM32G031", file=sys.stderr)
        return 1

    bases, regs = {}, {}
    for p in root.iter('peripheral'):
        n = p.findtext('name')
        if n not in WANT:
            continue
        if p.findtext('baseAddress'):
            bases[n] = p.findtext('baseAddress')
        if WANT[n]:
            regs[n] = sorted(
                ((r.findtext('name'), r.findtext('addressOffset'))
                 for r in p.iter('register') if r.findtext('name') in WANT[n]),
                key=lambda x: int(x[1], 16))

    missing = [f'{p}.{r}' for p, rs in WANT.items() for r in rs
               if r not in {x[0] for x in regs.get(p, [])}]
    if missing:
        print('not found in the SVD: ' + ', '.join(missing), file=sys.stderr)
        return 1

    out = [HEADER,
           '/* --- peripheral bases --------------------------------------------------- */']
    for n in sorted(bases):
        out.append(f'#define {n + "_BASE":<14} {bases[n]}u')
    out.append('')
    for n in sorted(regs):
        out.append(f'/* --- {n} ---------------------------------------------------------- */')
        for rn, off in regs[n]:
            out.append(f'#define {n + "_" + rn:<16} SUP_REG32({n}_BASE + {off}u)')
        out.append('')
    out.append('#endif /* STM32G031_H */')
    OUT.write_text('\n'.join(out) + '\n')
    print(f'wrote {OUT} ({len(bases)} peripherals, '
          f'{sum(len(v) for v in regs.values())} registers)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
