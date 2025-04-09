/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#ifndef _BITVECTOR_H
#define _BITVECTOR_H

// This functionally defined the total number of possible bits.
// Anything that is < (2 ^ BITVECTOR_NIBBLE_SIZE) is acceptable.
// Seems kind of arbitrary, but I'm sure there is a reason.... maybe some weird optimization....
static const size_t BITVECTOR_NIBBLE_SIZE = 8;

void BitVector_SetBit(uint8 *vector, int *offset);
void BitVector_ClearBit(uint8 *vector, int *offset);
void BitVector_WriteNibblet(uint8 *vector, int nibble, int *offset);
int BitVector_ReadBit(uint8 *vector, int *offset);
int BitVector_ReadNibblet(uint8 *vector, int *offset);

#endif // _BITVECTOR_H
