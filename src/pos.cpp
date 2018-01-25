// Copyright (c) 2012-2013 The PPCoin developers
// Copyright (c) 2014 The BlackCoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/assign/list_of.hpp>

#include "pos.h"
#include "txdb.h"
#include "main.h"
#include "arith_uint256.h"
#include "hash.h"
#include "timedata.h"
#include "chainparams.h"
#include "script/sign.h"

using namespace std;

// Get selection interval section (in seconds)
static int64_t GetStakeModifierSelectionIntervalSection(int nSection)
{
    assert (nSection >= 0 && nSection < 64);
    return (nModifierInterval * 63 / (63 + ((63 - nSection) * (MODIFIER_INTERVAL_RATIO - 1))));
}

// Get stake modifier selection interval (in seconds)
static int64_t GetStakeModifierSelectionInterval()
{
    int64_t nSelectionInterval = 0;
    for (int nSection=0; nSection<64; nSection++)
        nSelectionInterval += GetStakeModifierSelectionIntervalSection(nSection);
    return nSelectionInterval;
}

// select a block from the candidate blocks in vSortedByTimestamp, excluding
// already selected blocks in vSelectedBlocks, and with timestamp up to
// nSelectionIntervalStop.
static bool SelectBlockFromCandidates(vector<pair<int64_t, uint256> >& vSortedByTimestamp, map<uint256, const CBlockIndex*>& mapSelectedBlocks,
    int64_t nSelectionIntervalStop, uint64_t nStakeModifierPrev, const CBlockIndex** pindexSelected)
{
    bool fSelected = false;
    uint256 hashBest;
    *pindexSelected = (const CBlockIndex*) 0;
    BOOST_FOREACH(const PAIRTYPE(int64_t, uint256)& item, vSortedByTimestamp)
    {
        if (!mapBlockIndex.count(item.second))
            return error("SelectBlockFromCandidates: failed to find block index for candidate block %s", item.second.ToString());
        const CBlockIndex* pindex = mapBlockIndex[item.second];
        if (fSelected && pindex->GetBlockTime() > nSelectionIntervalStop)
            break;
        if (mapSelectedBlocks.count(pindex->GetBlockHash()) > 0)
            continue;
        // compute the selection hash by hashing its proof-hash and the
        // previous proof-of-stake modifier
        CDataStream ss(SER_GETHASH, 0);
        ss << pindex->hashProof << nStakeModifierPrev;
        uint256 hashSelection = Hash(ss.begin(), ss.end());
        // the selection hash is divided by 2**32 so that proof-of-stake block
        // is always favored over proof-of-work block. this is to preserve
        // the energy efficiency property
        if (pindex->IsProofOfStake())
        {
            arith_uint256 arithSelection = UintToArith256(hashSelection);
            arithSelection >>= 32;
            hashSelection = ArithToUint256(arithSelection);
        }
        if (fSelected && hashSelection < hashBest)
        {
            hashBest = hashSelection;
            *pindexSelected = (const CBlockIndex*) pindex;
        }
        else if (!fSelected)
        {
            fSelected = true;
            hashBest = hashSelection;
            *pindexSelected = (const CBlockIndex*) pindex;
        }
    }
    LogPrint("stakemodifier", "SelectBlockFromCandidates: selection hash=%s\n", hashBest.ToString());
    return fSelected;
}

// Stake Modifier (hash modifier of proof-of-stake):
// The purpose of stake modifier is to prevent a txout (coin) owner from
// computing future proof-of-stake generated by this txout at the time
// of transaction confirmation. To meet kernel protocol, the txout
// must hash with a future stake modifier to generate the proof.
uint256 ComputeStakeModifierV2(const CBlockIndex* pindexPrev, const uint256& kernel)
{
    if (!pindexPrev)
        return uint256();  // genesis block's modifier is 0

    CDataStream ss(SER_GETHASH, 0);
    ss << kernel << pindexPrev->bnStakeModifierV2;
    return Hash(ss.begin(), ss.end());
}

// BlackCoin kernel protocol
// coinstake must meet hash target according to the protocol:
// kernel (input 0) must meet the formula
//     hash(nStakeModifier + txPrev.block.nTime + txPrev.nTime + txPrev.vout.hash + txPrev.vout.n + nTime) < bnTarget * nWeight
// this ensures that the chance of getting a coinstake is proportional to the
// amount of coins one owns.
// The reason this hash is chosen is the following:
//   nStakeModifier: scrambles computation to make it very difficult to precompute
//                   future proof-of-stake
//   txPrev.block.nTime: prevent nodes from guessing a good timestamp to
//                       generate transaction for future advantage,
//                       obsolete since v3
//   txPrev.vout.hash: hash of txPrev, to reduce the chance of nodes
//                     generating coinstake at the same time
//   txPrev.vout.n: output number of txPrev, to reduce the chance of nodes
//                  generating coinstake at the same time
//   nTime: current timestamp
//   block/tx hash should not be used here as they can be generated in vast
//   quantities so as to generate blocks faster, degrading the system back into
//   a proof-of-work situation.
//
static bool CheckStakeKernelHashV2(CBlockIndex* pindexPrev, unsigned int nBits, unsigned int nTimeBlockFrom, const CTransaction& txPrev, const COutPoint& prevout, unsigned int nTime, uint256& hashProofOfStake, uint256& targetProofOfStake, bool fPrintProofOfStake)
{
    uint32_t nStakeTime = nTimeBlockFrom & ~STAKE_TIMESTAMP_MASK;

    if (nTime < nStakeTime)
        return error("CheckStakeKernelHash() : nTime violation");

    // Base target
    arith_uint256 bnTarget;
    bnTarget.SetCompact(nBits);

    // Weighted target
    int64_t nValueIn = txPrev.vout[prevout.n].nValue;
    arith_uint256 bnWeight = arith_uint256(nValueIn);
    bnTarget *= bnWeight;

    targetProofOfStake = ArithToUint256(bnTarget);

    uint256 bnStakeModifierV2 = pindexPrev->bnStakeModifierV2;
    int nStakeModifierHeight = pindexPrev->nHeight;
    int64_t nStakeModifierTime = nStakeTime;

    // Calculate hash
    CDataStream ss(SER_GETHASH, 0);
    ss << bnStakeModifierV2;
    ss << nStakeModifierTime << prevout.hash << prevout.n << nTime;
    hashProofOfStake = Hash(ss.begin(), ss.end());

    if (fPrintProofOfStake)
    {
        LogPrintf("CheckStakeKernelHash() : using block at height=%d timestamp=%s for block from timestamp=%s\n",
            nStakeModifierHeight,
            DateTimeStrFormat(nStakeModifierTime),
            DateTimeStrFormat(nTimeBlockFrom));
        LogPrintf("CheckStakeKernelHash() : check nTimeBlockFrom=%u nStakeTime=%u nPrevout=%u nTimeTx=%u hashProof=%s\n",
            nTimeBlockFrom, nStakeModifierTime, prevout.n, nTime,
            hashProofOfStake.ToString());
    }

    // Now check if proof-of-stake hash meets target protocol
    if (UintToArith256(hashProofOfStake) > bnTarget)
        return false;

    if (fDebug && !fPrintProofOfStake)
    {
        LogPrintf("CheckStakeKernelHash() : using block at height=%d timestamp=%s for block from timestamp=%s\n",
            nStakeModifierHeight,
            DateTimeStrFormat(nStakeModifierTime),
            DateTimeStrFormat(nTimeBlockFrom));
        LogPrintf("CheckStakeKernelHash() : pass nTimeBlockFrom=%u nPrevout=%u nTimeTx=%u hashProof=%s\n",
            nTimeBlockFrom, prevout.n, nTime,
            hashProofOfStake.ToString());
    }

    return true;
}

bool CheckStakeKernelHash(CBlockIndex* pindexPrev, unsigned int nBits, const CBlockHeader& blockFrom, unsigned int nTxPrevOffset, const CTransaction& txPrev, const COutPoint& prevout, unsigned int nTimeTx, uint256& hashProofOfStake, uint256& targetProofOfStake, bool fPrintProofOfStake)
{
    return CheckStakeKernelHashV2(pindexPrev, nBits, blockFrom.GetBlockTime(), txPrev, prevout, nTimeTx, hashProofOfStake, targetProofOfStake, fPrintProofOfStake);
}

// Check kernel hash target and coinstake signature
bool CheckProofOfStake(CBlockIndex* pindexPrev, CValidationState& state, const CTransaction& tx, unsigned int nTime, unsigned int nBits, uint256& hashProofOfStake, uint256& targetProofOfStake)
{
    if (!tx.IsCoinStake())
        return error("CheckProofOfStake() : called on non-coinstake %s", tx.GetHash().ToString());

    // Kernel (input 0) must match the stake hash target per coin age (nBits)
    const CTxIn& txin = tx.vin[0];

    // First try finding the previous transaction in database
    CTransaction txPrev;
    CDiskTxPos txindex;
    if (!ReadFromDisk(txPrev, txindex, *pblocktree, txin.prevout))
        return state.DoS(1, error("CheckProofOfStake() : INFO: read txPrev failed"));  // previous transaction not in main chain, may occur during initial download

    // Verify signature
    if (!VerifySignature(txPrev, tx, 0, SCRIPT_VERIFY_NONE))
        return state.DoS(100, error("CheckProofOfStake() : VerifySignature failed on coinstake %s", tx.GetHash().ToString()));

    // Read block header
    CBlockHeader block;
    if (!ReadFromDisk(block, txindex.nFile, txindex.nPos))
        return fDebug? error("CheckProofOfStake() : read block failed") : false; // unable to read block of previous transaction

    // Min age requirement
    int nDepth;
    if (IsConfirmedInNPrevBlocks(txindex, pindexPrev, nStakeMinConfirmations - 1, nDepth))
        return state.DoS(100, error("CheckProofOfStake() : tried to stake at depth %d", nDepth + 1));

    if (!CheckStakeKernelHash(pindexPrev, nBits, block, txindex.nTxOffset - txindex.nPos, txPrev, txin.prevout, nTime, hashProofOfStake, targetProofOfStake, fDebug))
        return state.DoS(1, error("CheckProofOfStake() : INFO: check kernel failed on coinstake %s, hashProof=%s", tx.GetHash().ToString(), hashProofOfStake.ToString())); // may occur during initial download or if behind on block chain sync

    return true;
}

// Check whether the coinstake timestamp meets protocol
bool CheckCoinStakeTimestamp(int64_t nTimeBlock, int64_t nTimeTx)
{
    return (nTimeBlock == nTimeTx) && ((nTimeTx & STAKE_TIMESTAMP_MASK) == 0);
}

bool CheckKernel(CBlockIndex* pindexPrev, unsigned int nBits, uint32_t nTime, const COutPoint& prevout, int64_t* pBlockTime)
{
    uint256 hashProofOfStake, targetProofOfStake;

    CTransaction txPrev;
    CDiskTxPos txindex;
    if (!ReadFromDisk(txPrev, txindex, *pblocktree, prevout))
        return false;

    // Read block header
    CBlockHeader block;
    if (!ReadFromDisk(block, txindex.nFile, txindex.nPos))
        return false;

    int nDepth;
    if (IsConfirmedInNPrevBlocks(txindex, pindexPrev, nStakeMinConfirmations - 1, nDepth))
        return false;

    if (pBlockTime)
        *pBlockTime = block.GetBlockTime();

    return CheckStakeKernelHash(pindexPrev, nBits, block, txindex.nTxOffset - txindex.nPos, txPrev, prevout, nTime, hashProofOfStake, targetProofOfStake);
}
