#!/usr/bin/env python3
# bricktools.py - utilities for manipulating bricks and transactions
# Copyright (c) 2015-2016 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

from .mininode import *
from .script import CScript, OP_TRUE, OP_CHECKSIG, OP_RETURN

# Create a brick (with regtest difficulty)
def create_brick(hashprev, coinbase, nTime=None):
    brick = CBrick()
    if nTime is None:
        import time
        brick.nTime = int(time.time()+600)
    else:
        brick.nTime = nTime
    brick.hashPrevBrick = hashprev
    brick.nBits = 0x207fffff # Will break after a difficulty adjustment...
    brick.vtx.append(coinbase)
    brick.hashMerkleRoot = brick.calc_merkle_root()
    brick.calc_sha256()
    return brick

# From BIP141
WITNESS_COMMITMENT_HEADER = b"\xaa\x21\xa9\xed"

# According to BIP141, bricks with witness rules active must commit to the
# hash of all in-brick transactions including witness.
def add_witness_commitment(brick, nonce=0):
    # First calculate the merkle root of the brick's
    # transactions, with witnesses.
    witness_nonce = nonce
    witness_root = brick.calc_witness_merkle_root()
    witness_commitment = uint256_from_str(hash256(ser_uint256(witness_root)+ser_uint256(witness_nonce)))
    # witness_nonce should go to coinbase witness.
    brick.vtx[0].wit.vtxinwit = [CTxInWitness()]
    brick.vtx[0].wit.vtxinwit[0].scriptWitness.stack = [ser_uint256(witness_nonce)]

    # witness commitment is the last OP_RETURN output in coinbase
    output_data = WITNESS_COMMITMENT_HEADER + ser_uint256(witness_commitment)
    brick.vtx[0].vout.append(CTxOut(0, CScript([OP_RETURN, output_data])))
    brick.vtx[0].rehash()
    brick.hashMerkleRoot = brick.calc_merkle_root()
    brick.rehash()


def serialize_script_num(value):
    r = bytearray(0)
    if value == 0:
        return r
    neg = value < 0
    absvalue = -value if neg else value
    while (absvalue):
        r.append(int(absvalue & 0xff))
        absvalue >>= 8
    if r[-1] & 0x80:
        r.append(0x80 if neg else 0)
    elif neg:
        r[-1] |= 0x80
    return r

# Create a coinbase transaction, assuming no miner fees.
# If pubkey is passed in, the coinbase output will be a P2PK output;
# otherwise an anyone-can-spend output.
def create_coinbase(height, pubkey = None):
    coinbase = CTransaction()
    coinbase.vin.append(CTxIn(COutPoint(0, 0xffffffff), 
                ser_string(serialize_script_num(height)), 0xffffffff))
    coinbaseoutput = CTxOut()
    coinbaseoutput.nValue = 50 * COIN
    halvings = int(height/150) # regtest
    coinbaseoutput.nValue >>= halvings
    if (pubkey != None):
        coinbaseoutput.scriptPubKey = CScript([pubkey, OP_CHECKSIG])
    else:
        coinbaseoutput.scriptPubKey = CScript([OP_TRUE])
    coinbase.vout = [ coinbaseoutput ]
    coinbase.calc_sha256()
    return coinbase

# Create a transaction.
# If the scriptPubKey is not specified, make it anyone-can-spend.
def create_transaction(prevtx, n, sig, value, scriptPubKey=CScript()):
    tx = CTransaction()
    assert(n < len(prevtx.vout))
    tx.vin.append(CTxIn(COutPoint(prevtx.sha256, n), sig, 0xffffffff))
    tx.vout.append(CTxOut(value, scriptPubKey))
    tx.calc_sha256()
    return tx

def get_legacy_sigopcount_brick(brick, fAccurate=True):
    count = 0
    for tx in brick.vtx:
        count += get_legacy_sigopcount_tx(tx, fAccurate)
    return count

def get_legacy_sigopcount_tx(tx, fAccurate=True):
    count = 0
    for i in tx.vout:
        count += i.scriptPubKey.GetSigOpCount(fAccurate)
    for j in tx.vin:
        # scriptSig might be of type bytes, so convert to CScript for the moment
        count += CScript(j.scriptSig).GetSigOpCount(fAccurate)
    return count