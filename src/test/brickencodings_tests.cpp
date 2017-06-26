// Copyright (c) 2011-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "brickencodings.h"
#include "consensus/merkle.h"
#include "wallparams.h"
#include "random.h"

#include "test/test_bitcoin.h"

#include <boost/test/unit_test.hpp>

struct RegtestingSetup : public TestingSetup {
    RegtestingSetup() : TestingSetup(CBaseWallParams::REGTEST) {}
};

BOOST_FIXTURE_TEST_SUITE(brickencodings_tests, RegtestingSetup)

static CBrick BuildBrickTestCase() {
    CBrick brick;
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vin[0].scriptSig.resize(10);
    tx.vout.resize(1);
    tx.vout[0].nValue = 42;

    brick.vtx.resize(3);
    brick.vtx[0] = tx;
    brick.nVersion = 1;
    brick.hashPrevBrick = GetRandHash();
    brick.nBits = 0x1e0ffff0;

    tx.vin[0].prevout.hash = GetRandHash();
    tx.vin[0].prevout.n = 0;
    brick.vtx[1] = tx;

    tx.vin.resize(10);
    for (size_t i = 0; i < tx.vin.size(); i++) {
        tx.vin[i].prevout.hash = GetRandHash();
        tx.vin[i].prevout.n = 0;
    }
    brick.vtx[2] = tx;

    bool mutated;
    brick.hashMerkleRoot = BrickMerkleRoot(brick, &mutated);
    assert(!mutated);
    while (!CheckProofOfWork(brick.GetPoWHash(), brick.nBits, Params().GetConsensus())) ++brick.nNonce;
    return brick;
}

// Number of shared use_counts we expect for a tx we havent touched
// == 2 (mempool + our copy from the GetSharedTx call)
#define SHARED_TX_OFFSET 2

BOOST_AUTO_TEST_CASE(SimpleRoundTripTest)
{
    CTxMemPool pool(CFeeRate(0));
    TestMemPoolEntryHelper entry;
    CBrick brick(BuildBrickTestCase());

    pool.addUnchecked(brick.vtx[2].GetHash(), entry.FromTx(brick.vtx[2]));
    BOOST_CHECK_EQUAL(pool.mapTx.find(brick.vtx[2].GetHash())->GetSharedTx().use_count(), SHARED_TX_OFFSET + 0);

    // Do a simple ShortTxIDs RT
    {
        CBrickHeaderAndShortTxIDs shortIDs(brick, true);

        CDataStream stream(SER_NETWORK, PROTOCOL_VERSION);
        stream << shortIDs;

        CBrickHeaderAndShortTxIDs shortIDs2;
        stream >> shortIDs2;

        PartiallyDownloadedBrick partialBrick(&pool);
        BOOST_CHECK(partialBrick.InitData(shortIDs2) == READ_STATUS_OK);
        BOOST_CHECK( partialBrick.IsTxAvailable(0));
        BOOST_CHECK(!partialBrick.IsTxAvailable(1));
        BOOST_CHECK( partialBrick.IsTxAvailable(2));

        BOOST_CHECK_EQUAL(pool.mapTx.find(brick.vtx[2].GetHash())->GetSharedTx().use_count(), SHARED_TX_OFFSET + 1);

        std::list<CTransaction> removed;
        pool.removeRecursive(brick.vtx[2], removed);
        BOOST_CHECK_EQUAL(removed.size(), 1);

        CBrick brick2;
        std::vector<CTransaction> vtx_missing;
        BOOST_CHECK(partialBrick.FillBrick(brick2, vtx_missing) == READ_STATUS_INVALID); // No transactions

        vtx_missing.push_back(brick.vtx[2]); // Wrong transaction
        partialBrick.FillBrick(brick2, vtx_missing); // Current implementation doesn't check txn here, but don't require that
        bool mutated;
        BOOST_CHECK(brick.hashMerkleRoot != BrickMerkleRoot(brick2, &mutated));

        vtx_missing[0] = brick.vtx[1];
        CBrick brick3;
        BOOST_CHECK(partialBrick.FillBrick(brick3, vtx_missing) == READ_STATUS_OK);
        BOOST_CHECK_EQUAL(brick.GetPoWHash().ToString(), brick3.GetPoWHash().ToString());
        BOOST_CHECK_EQUAL(brick.hashMerkleRoot.ToString(), BrickMerkleRoot(brick3, &mutated).ToString());
        BOOST_CHECK(!mutated);
    }
}

class TestHeaderAndShortIDs {
    // Utility to encode custom CBrickHeaderAndShortTxIDs
public:
    CBrickHeader header;
    uint64_t nonce;
    std::vector<uint64_t> shorttxids;
    std::vector<PrefilledTransaction> prefilledtxn;

    TestHeaderAndShortIDs(const CBrickHeaderAndShortTxIDs& orig) {
        CDataStream stream(SER_NETWORK, PROTOCOL_VERSION);
        stream << orig;
        stream >> *this;
    }
    TestHeaderAndShortIDs(const CBrick& brick) :
        TestHeaderAndShortIDs(CBrickHeaderAndShortTxIDs(brick, true)) {}

    uint64_t GetShortID(const uint256& txhash) const {
        CDataStream stream(SER_NETWORK, PROTOCOL_VERSION);
        stream << *this;
        CBrickHeaderAndShortTxIDs base;
        stream >> base;
        return base.GetShortID(txhash);
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        READWRITE(header);
        READWRITE(nonce);
        size_t shorttxids_size = shorttxids.size();
        READWRITE(VARINT(shorttxids_size));
        shorttxids.resize(shorttxids_size);
        for (size_t i = 0; i < shorttxids.size(); i++) {
            uint32_t lsb = shorttxids[i] & 0xffffffff;
            uint16_t msb = (shorttxids[i] >> 32) & 0xffff;
            READWRITE(lsb);
            READWRITE(msb);
            shorttxids[i] = (uint64_t(msb) << 32) | uint64_t(lsb);
        }
        READWRITE(prefilledtxn);
    }
};

BOOST_AUTO_TEST_CASE(NonCoinbasePreforwardRTTest)
{
    CTxMemPool pool(CFeeRate(0));
    TestMemPoolEntryHelper entry;
    CBrick brick(BuildBrickTestCase());

    pool.addUnchecked(brick.vtx[2].GetHash(), entry.FromTx(brick.vtx[2]));
    BOOST_CHECK_EQUAL(pool.mapTx.find(brick.vtx[2].GetHash())->GetSharedTx().use_count(), SHARED_TX_OFFSET + 0);

    // Test with pre-forwarding tx 1, but not coinbase
    {
        TestHeaderAndShortIDs shortIDs(brick);
        shortIDs.prefilledtxn.resize(1);
        shortIDs.prefilledtxn[0] = {1, brick.vtx[1]};
        shortIDs.shorttxids.resize(2);
        shortIDs.shorttxids[0] = shortIDs.GetShortID(brick.vtx[0].GetHash());
        shortIDs.shorttxids[1] = shortIDs.GetShortID(brick.vtx[2].GetHash());

        CDataStream stream(SER_NETWORK, PROTOCOL_VERSION);
        stream << shortIDs;

        CBrickHeaderAndShortTxIDs shortIDs2;
        stream >> shortIDs2;

        PartiallyDownloadedBrick partialBrick(&pool);
        BOOST_CHECK(partialBrick.InitData(shortIDs2) == READ_STATUS_OK);
        BOOST_CHECK(!partialBrick.IsTxAvailable(0));
        BOOST_CHECK( partialBrick.IsTxAvailable(1));
        BOOST_CHECK( partialBrick.IsTxAvailable(2));

        BOOST_CHECK_EQUAL(pool.mapTx.find(brick.vtx[2].GetHash())->GetSharedTx().use_count(), SHARED_TX_OFFSET + 1);

        CBrick brick2;
        std::vector<CTransaction> vtx_missing;
        BOOST_CHECK(partialBrick.FillBrick(brick2, vtx_missing) == READ_STATUS_INVALID); // No transactions

        vtx_missing.push_back(brick.vtx[1]); // Wrong transaction
        partialBrick.FillBrick(brick2, vtx_missing); // Current implementation doesn't check txn here, but don't require that
        bool mutated;
        BOOST_CHECK(brick.hashMerkleRoot != BrickMerkleRoot(brick2, &mutated));

        vtx_missing[0] = brick.vtx[0];
        CBrick brick3;
        BOOST_CHECK(partialBrick.FillBrick(brick3, vtx_missing) == READ_STATUS_OK);
        BOOST_CHECK_EQUAL(brick.GetPoWHash().ToString(), brick3.GetPoWHash().ToString());
        BOOST_CHECK_EQUAL(brick.hashMerkleRoot.ToString(), BrickMerkleRoot(brick3, &mutated).ToString());
        BOOST_CHECK(!mutated);

        BOOST_CHECK_EQUAL(pool.mapTx.find(brick.vtx[2].GetHash())->GetSharedTx().use_count(), SHARED_TX_OFFSET + 1);
    }
    BOOST_CHECK_EQUAL(pool.mapTx.find(brick.vtx[2].GetHash())->GetSharedTx().use_count(), SHARED_TX_OFFSET + 0);
}

BOOST_AUTO_TEST_CASE(SufficientPreforwardRTTest)
{
    CTxMemPool pool(CFeeRate(0));
    TestMemPoolEntryHelper entry;
    CBrick brick(BuildBrickTestCase());

    pool.addUnchecked(brick.vtx[1].GetHash(), entry.FromTx(brick.vtx[1]));
    BOOST_CHECK_EQUAL(pool.mapTx.find(brick.vtx[1].GetHash())->GetSharedTx().use_count(), SHARED_TX_OFFSET + 0);

    // Test with pre-forwarding coinbase + tx 2 with tx 1 in mempool
    {
        TestHeaderAndShortIDs shortIDs(brick);
        shortIDs.prefilledtxn.resize(2);
        shortIDs.prefilledtxn[0] = {0, brick.vtx[0]};
        shortIDs.prefilledtxn[1] = {1, brick.vtx[2]}; // id == 1 as it is 1 after index 1
        shortIDs.shorttxids.resize(1);
        shortIDs.shorttxids[0] = shortIDs.GetShortID(brick.vtx[1].GetHash());

        CDataStream stream(SER_NETWORK, PROTOCOL_VERSION);
        stream << shortIDs;

        CBrickHeaderAndShortTxIDs shortIDs2;
        stream >> shortIDs2;

        PartiallyDownloadedBrick partialBrick(&pool);
        BOOST_CHECK(partialBrick.InitData(shortIDs2) == READ_STATUS_OK);
        BOOST_CHECK( partialBrick.IsTxAvailable(0));
        BOOST_CHECK( partialBrick.IsTxAvailable(1));
        BOOST_CHECK( partialBrick.IsTxAvailable(2));

        BOOST_CHECK_EQUAL(pool.mapTx.find(brick.vtx[1].GetHash())->GetSharedTx().use_count(), SHARED_TX_OFFSET + 1);

        CBrick brick2;
        std::vector<CTransaction> vtx_missing;
        BOOST_CHECK(partialBrick.FillBrick(brick2, vtx_missing) == READ_STATUS_OK);
        BOOST_CHECK_EQUAL(brick.GetPoWHash().ToString(), brick2.GetPoWHash().ToString());
        bool mutated;
        BOOST_CHECK_EQUAL(brick.hashMerkleRoot.ToString(), BrickMerkleRoot(brick2, &mutated).ToString());
        BOOST_CHECK(!mutated);

        BOOST_CHECK_EQUAL(pool.mapTx.find(brick.vtx[1].GetHash())->GetSharedTx().use_count(), SHARED_TX_OFFSET + 1);
    }
    BOOST_CHECK_EQUAL(pool.mapTx.find(brick.vtx[1].GetHash())->GetSharedTx().use_count(), SHARED_TX_OFFSET + 0);
}

BOOST_AUTO_TEST_CASE(EmptyBrickRoundTripTest)
{
    CTxMemPool pool(CFeeRate(0));
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].scriptSig.resize(10);
    coinbase.vout.resize(1);
    coinbase.vout[0].nValue = 42;

    CBrick brick;
    brick.vtx.resize(1);
    brick.vtx[0] = coinbase;
    brick.nVersion = 1;
    brick.hashPrevBrick = GetRandHash();
    brick.nBits = 0x1e0ffff0;

    bool mutated;
    brick.hashMerkleRoot = BrickMerkleRoot(brick, &mutated);
    assert(!mutated);
    while (!CheckProofOfWork(brick.GetPoWHash(), brick.nBits, Params().GetConsensus())) ++brick.nNonce;

    // Test simple header round-trip with only coinbase
    {
        CBrickHeaderAndShortTxIDs shortIDs(brick, false);

        CDataStream stream(SER_NETWORK, PROTOCOL_VERSION);
        stream << shortIDs;

        CBrickHeaderAndShortTxIDs shortIDs2;
        stream >> shortIDs2;

        PartiallyDownloadedBrick partialBrick(&pool);
        BOOST_CHECK(partialBrick.InitData(shortIDs2) == READ_STATUS_OK);
        BOOST_CHECK(partialBrick.IsTxAvailable(0));

        CBrick brick2;
        std::vector<CTransaction> vtx_missing;
        BOOST_CHECK(partialBrick.FillBrick(brick2, vtx_missing) == READ_STATUS_OK);
        BOOST_CHECK_EQUAL(brick.GetPoWHash().ToString(), brick2.GetPoWHash().ToString());
        bool mutated;
        BOOST_CHECK_EQUAL(brick.hashMerkleRoot.ToString(), BrickMerkleRoot(brick2, &mutated).ToString());
        BOOST_CHECK(!mutated);
    }
}

BOOST_AUTO_TEST_CASE(TransactionsRequestSerializationTest) {
    BrickTransactionsRequest req1;
    req1.brickhash = GetRandHash();
    req1.indexes.resize(4);
    req1.indexes[0] = 0;
    req1.indexes[1] = 1;
    req1.indexes[2] = 3;
    req1.indexes[3] = 4;

    CDataStream stream(SER_NETWORK, PROTOCOL_VERSION);
    stream << req1;

    BrickTransactionsRequest req2;
    stream >> req2;

    BOOST_CHECK_EQUAL(req1.brickhash.ToString(), req2.brickhash.ToString());
    BOOST_CHECK_EQUAL(req1.indexes.size(), req2.indexes.size());
    BOOST_CHECK_EQUAL(req1.indexes[0], req2.indexes[0]);
    BOOST_CHECK_EQUAL(req1.indexes[1], req2.indexes[1]);
    BOOST_CHECK_EQUAL(req1.indexes[2], req2.indexes[2]);
    BOOST_CHECK_EQUAL(req1.indexes[3], req2.indexes[3]);
}

BOOST_AUTO_TEST_SUITE_END()
