// Copyright (c) 2012-2014 The Bitcoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "coins.h"

#include "random.h"
#include "consensus/consensus.h"
#include "memusage.h"

#include <assert.h>

/**
 * calculate number of bytes for the bitmask, and its number of non-zero bytes
 * each bit in the bitmask represents the availability of one output, but the
 * availabilities of the first two outputs are encoded separately
 */
void CCoins::CalcMaskSize(unsigned int& nBytes, unsigned int& nNonzeroBytes) const
{
    unsigned int nLastUsedByte = 0;
    for (unsigned int b = 0; 2 + b * 8 < vout.size(); b++) {
        bool fZero = true;
        for (unsigned int i = 0; i < 8 && 2 + b * 8 + i < vout.size(); i++) {
            if (!vout[2 + b * 8 + i].IsNull()) {
                fZero = false;
                continue;
            }
        }
        if (!fZero) {
            nLastUsedByte = b + 1;
            nNonzeroBytes++;
        }
    }
    nBytes += nLastUsedByte;
}

bool CCoins::Spend(uint32_t nPos)
{
    if (nPos >= vout.size() || vout[nPos].IsNull())
        return false;
    vout[nPos].SetNull();
    Cleanup();
    return true;
}

bool CCoinsView::GetCoin(const uint256& txid, CCoins& coins) const { return false; }
bool CCoinsView::HaveCoin(const uint256& txid) const { return false; }
uint256 CCoinsView::GetBestBlock() const { return uint256(0); }
bool CCoinsView::BatchWrite(CCoinsMap& mapCoins, const uint256& hashBlock) { return false; }
bool CCoinsView::GetStats(CCoinsStats& stats) const { return false; }

CCoinsViewBacked::CCoinsViewBacked(CCoinsView* viewIn) : base(viewIn) {}
bool CCoinsViewBacked::GetCoin(const uint256& txid, CCoins& coins) const
{
   if (!base) return false;
   return base->GetCoin(txid, coins);
}

bool CCoinsViewBacked::HaveCoin(const uint256& txid) const
{
    if (!base) return false;
    return base->HaveCoin(txid);
}

uint256 CCoinsViewBacked::GetBestBlock() const
{
    if (!base) return false;
    return base->GetBestBlock();
}

void CCoinsViewBacked::SetBackend(CCoinsView& viewIn)
{
    base = &viewIn;
}

bool CCoinsViewBacked::BatchWrite(CCoinsMap& mapCoins, const uint256& hashBlock)
{
    if (!base) return false;
    return base->BatchWrite(mapCoins, hashBlock);
}

bool CCoinsViewBacked::GetStats(CCoinsStats& stats) const
{
    if (!base) return false;
    return base->GetStats(stats);
}

CCoinsKeyHasher::CCoinsKeyHasher() : salt(GetRandHash()) {}

CCoinsViewCache::CCoinsViewCache(CCoinsView* baseIn) : CCoinsViewBacked(baseIn), hasModifier(false), hashBlock(0), cachedCoinsUsage(0) {}

CCoinsViewCache::~CCoinsViewCache()
{
    assert(!hasModifier);
}

size_t CCoinsViewCache::DynamicMemoryUsage() const {
    return memusage::DynamicUsage(cacheCoins) + cachedCoinsUsage;
}

CCoinsMap::iterator CCoinsViewCache::FetchCoin(const uint256& txid) const
{
    CCoinsMap::iterator it = cacheCoins.find(txid);
    if (it != cacheCoins.end())
        return it;
    CCoins tmp;
    if (!base->GetCoin(txid, tmp))
        return cacheCoins.end();
    CCoinsMap::iterator ret = cacheCoins.insert(std::make_pair(txid, CCoinsCacheEntry())).first;
    tmp.swap(ret->second.coins);
    if (ret->second.coins.IsSpent()) {
        // The parent only has an empty entry for this txid; we can consider our
        // version as fresh.
        ret->second.flags = CCoinsCacheEntry::FRESH;
    }
    cachedCoinsUsage += ret->second.coins.DynamicMemoryUsage();
    return ret;
}

bool CCoinsViewCache::GetCoin(const uint256& txid, CCoins& coins) const
{
    CCoinsMap::const_iterator it = FetchCoin(txid);
    if (it != cacheCoins.end()) {
        coins = it->second.coins;
        return !coins.IsSpent();
    }
    return false;
}

CCoinsModifier CCoinsViewCache::ModifyCoins(const uint256& txid)
{
    assert(!hasModifier);
    std::pair<CCoinsMap::iterator, bool> ret = cacheCoins.insert(std::make_pair(txid, CCoinsCacheEntry()));
    size_t cachedCoinUsage = 0;
    if (ret.second) {
        if (!base->GetCoin(txid, ret.first->second.coins)) {
            // The parent view does not have this entry; mark it as fresh.
            ret.first->second.coins.Clear();
            // New coins must not already exist.
            if (!ret.first->second.coins.IsSpent())
                throw std::logic_error("ModifyNewCoins should not find pre-existing coins on a non-coinbase unless they are pruned!");

            if (!(ret.first->second.flags & CCoinsCacheEntry::FRESH)) {
                // If the coin is known to be pruned (have no unspent outputs) in
                // the current view and the cache entry is not dirty, we know the
                // coin also must be pruned in the parent view as well, so it is safe
                // to mark this fresh.
                ret.first->second.flags |= CCoinsCacheEntry::FRESH;
            }
        }
    } else {
        cachedCoinUsage = ret.first->second.coins.DynamicMemoryUsage();
    }
    // Assume that whenever ModifyCoins is called, the entry will be modified.
    ret.first->second.flags |= CCoinsCacheEntry::DIRTY;
    return CCoinsModifier(*this, ret.first, cachedCoinUsage);
}
void CCoinsViewCache::AddCoin(const COutPoint &outpoint, CCoins&& coin, bool possible_overwrite) {
    assert(!coin.IsSpent());
    for (const CTxOut& out : coin.vout)
        if (out.scriptPubKey.IsUnspendable()) return;

    CCoinsMap::iterator it;
    bool inserted;
    std::tie(it, inserted) = cacheCoins.emplace(std::piecewise_construct, std::forward_as_tuple(outpoint.hash), std::tuple<>());
    bool fresh = false;
    if (!inserted) {
        cachedCoinsUsage -= it->second.coins.DynamicMemoryUsage();
    }
    if (!possible_overwrite) {
        if (it->second.coins.IsAvailable(outpoint.n)) {
            throw std::logic_error("Adding new coin that replaces non-pruned entry");
        }
        fresh = it->second.coins.IsSpent() && !(it->second.flags & CCoinsCacheEntry::DIRTY);
    }
    if (it->second.coins.vout.size() <= outpoint.n) {
        it->second.coins.vout.resize(outpoint.n + 1);
    }
    it->second.coins.vout[outpoint.n] = std::move(coin.vout[outpoint.n]);
    it->second.coins.nHeight = coin.nHeight;
    it->second.coins.fCoinBase = coin.fCoinBase;
    it->second.flags |= CCoinsCacheEntry::DIRTY | (fresh ? CCoinsCacheEntry::FRESH : 0);
    cachedCoinsUsage += it->second.coins.DynamicMemoryUsage();
}

void AddCoins(CCoinsViewCache& cache, const CTransaction &tx, int nHeight) {
    bool fCoinbase = tx.IsCoinBase();
    const uint256& txid = tx.GetHash();
    for (size_t i = 0; i < tx.vout.size(); ++i) {
        // Pass fCoinbase as the possible_overwrite flag to AddCoin, in order to correctly
        // deal with the pre-BIP30 occurrances of duplicate coinbase transactions.
        cache.AddCoin(COutPoint(txid, i), CCoins(tx.vout[i], nHeight, fCoinbase), fCoinbase);
    }
}

void CCoinsViewCache::SpendCoin(const COutPoint &outpoint, CCoins* moveout) {
    CCoinsMap::iterator it = FetchCoin(outpoint.hash);
    if (it == cacheCoins.end()) return;
    cachedCoinsUsage -= it->second.coins.DynamicMemoryUsage();

    if (moveout && it->second.coins.IsAvailable(outpoint.n)) {
        *moveout = CCoins(it->second.coins.vout[outpoint.n], it->second.coins.nHeight, it->second.coins.fCoinBase);
    }
    it->second.coins.Spend(outpoint.n); // Ignore return value: SpendCoin has no effect if no UTXO found.
    if (it->second.coins.IsSpent() && it->second.flags & CCoinsCacheEntry::FRESH) {
        cacheCoins.erase(it);
    } else {
        it->second.flags |= CCoinsCacheEntry::DIRTY;
        it->second.coins.Clear();
    }
}

const CCoins* CCoinsViewCache::AccessCoins(const uint256& txid) const
{
    CCoinsMap::const_iterator it = FetchCoin(txid);
    if (it == cacheCoins.end()) {
        return nullptr;
    } else {
        return &it->second.coins;
    }
}

static const CCoins coinEmpty;

const CCoins CCoinsViewCache::AccessCoin(const COutPoint &outpoint) const {
    CCoinsMap::iterator it = FetchCoin(outpoint.hash);
    if (it == cacheCoins.end() || !it->second.coins.IsAvailable(outpoint.n)) {
        return coinEmpty;
    } else {
        return CCoins(it->second.coins.vout[outpoint.n], it->second.coins.nHeight, it->second.coins.fCoinBase);
    }
}

bool CCoinsViewCache::HaveCoin(const uint256& txid) const
{
    CCoinsMap::const_iterator it = FetchCoin(txid);
    // We're using vtx.empty() instead of IsSpent here for performance reasons,
    // as we only care about the case where a transaction was replaced entirely
    // in a reorganization (which wipes vout entirely, as opposed to spending
    // which just cleans individual outputs).
    return (it != cacheCoins.end() && !it->second.coins.vout.empty());
}

bool CCoinsViewCache::HaveCoin(const COutPoint &outpoint) const {
    CCoinsMap::iterator it = FetchCoin(outpoint.hash);
    return (it != cacheCoins.end() && it->second.coins.IsAvailable(outpoint.n));
}

uint256 CCoinsViewCache::GetBestBlock() const
{
    if (hashBlock == uint256(0))
        hashBlock = base->GetBestBlock();
    return hashBlock;
}

void CCoinsViewCache::SetBestBlock(const uint256& hashBlockIn)
{
    hashBlock = hashBlockIn;
}

bool CCoinsViewCache::BatchWrite(CCoinsMap& mapCoins, const uint256& hashBlockIn)
{
    for (CCoinsMap::iterator it = mapCoins.begin(); it != mapCoins.end(); it = mapCoins.erase(it)) {
        // Ignore non-dirty entries (optimization).
        if (!(it->second.flags & CCoinsCacheEntry::DIRTY)) {
            continue;
        }

        CCoinsMap::iterator itUs = cacheCoins.find(it->first);
        if (itUs == cacheCoins.end()) {
            // The parent cache does not have an entry, while the child does
            // We can ignore it if it's both FRESH and pruned in the child
            if (!(it->second.flags & CCoinsCacheEntry::FRESH && it->second.coins.IsSpent())) {
                // Otherwise we will need to create it in the parent
                // and move the data up and mark it as dirty

                CCoinsCacheEntry& entry = cacheCoins[it->first];
                entry.coins = std::move(it->second.coins);
                cachedCoinsUsage += entry.coins.DynamicMemoryUsage();
                entry.flags = CCoinsCacheEntry::DIRTY;
                // We can mark it FRESH in the parent if it was FRESH in the child
                // Otherwise it might have just been flushed from the parent's cache
                // and already exist in the grandparent
                if (it->second.flags & CCoinsCacheEntry::FRESH) {
                    entry.flags |= CCoinsCacheEntry::FRESH;
               }
            }
        } else {
            // Assert that the child cache entry was not marked FRESH if the
            // parent cache entry has unspent outputs. If this ever happens,
            // it means the FRESH flag was misapplied and there is a logic
            // error in the calling code.
            if ((it->second.flags & CCoinsCacheEntry::FRESH) && !itUs->second.coins.IsSpent()) {
                throw std::logic_error("FRESH flag misapplied to cache entry for base transaction with spendable outputs");
            }

            // Found the entry in the parent cache
            if ((itUs->second.flags & CCoinsCacheEntry::FRESH) && it->second.coins.IsSpent()) {
                // The grandparent does not have an entry, and the child is
                // modified and being pruned. This means we can just delete
                // it from the parent.
                cachedCoinsUsage -= itUs->second.coins.DynamicMemoryUsage();
                cacheCoins.erase(itUs);
            } else {
                // A normal modification.
                cachedCoinsUsage -= itUs->second.coins.DynamicMemoryUsage();
                itUs->second.coins = std::move(it->second.coins);
                cachedCoinsUsage += itUs->second.coins.DynamicMemoryUsage();
                itUs->second.flags |= CCoinsCacheEntry::DIRTY;
                // NOTE: It is possible the child has a FRESH flag here in
                // the event the entry we found in the parent is pruned. But
                // we must not copy that FRESH flag to the parent as that
                // pruned state likely still needs to be communicated to the
                // grandparent.
            }
        }
    }
    hashBlock = hashBlockIn;
    return true;
}

bool CCoinsViewCache::Flush()
{
    bool fOk = base->BatchWrite(cacheCoins, hashBlock);
    cacheCoins.clear();
    cachedCoinsUsage = 0;
    return fOk;
}

void CCoinsViewCache::Uncache(const COutPoint& outpoint)
{
    CCoinsMap::iterator it = FetchCoin(outpoint.hash);
    if (it != cacheCoins.end() && it->second.flags == 0) {
        cachedCoinsUsage -= it->second.coins.DynamicMemoryUsage();
        cacheCoins.erase(it);
    }
}

unsigned int CCoinsViewCache::GetCacheSize() const
{
    return cacheCoins.size();
}

const CTxOut& CCoinsViewCache::GetOutputFor(const CTxIn& input) const
{
    const CCoins* coins = AccessCoins(input.prevout.hash);
    assert(coins && coins->IsAvailable(input.prevout.n));
    return coins->vout[input.prevout.n];
}

CAmount CCoinsViewCache::GetValueIn(const CTransaction& tx) const
{
    if (tx.IsCoinBase())
        return 0;

    CAmount nResult = 0;
    for (unsigned int i = 0; i < tx.vin.size(); i++)
        nResult += GetOutputFor(tx.vin[i]).nValue;

    return nResult;
}

bool CCoinsViewCache::HaveInputs(const CTransaction& tx) const
{
    if (!tx.IsCoinBase()) {
        for (unsigned int i = 0; i < tx.vin.size(); i++) {
            const COutPoint& prevout = tx.vin[i].prevout;
            const CCoins* coins = AccessCoins(prevout.hash);
            if (!coins || !coins->IsAvailable(prevout.n)) {
                return false;
            }
        }
    }
    return true;
}

double CCoinsViewCache::GetPriority(const CTransaction& tx, int nHeight, CAmount &inChainInputValue) const
{
    inChainInputValue = 0;
    if (tx.IsCoinGenerated())
        return 0.0;
    double dResult = 0.0;
    for (const CTxIn& txin : tx.vin) {
        const CCoins* coins = AccessCoins(txin.prevout.hash);
        assert(coins);
        if (!coins->IsAvailable(txin.prevout.n)) continue;
        if (coins->nHeight < nHeight) {
            dResult += coins->vout[txin.prevout.n].nValue * (nHeight - coins->nHeight);
            inChainInputValue += coins->vout[txin.prevout.n].nValue;
        }
    }
    return tx.ComputePriority(dResult);
}

CCoinsModifier::CCoinsModifier(CCoinsViewCache& cache_, CCoinsMap::iterator it_, size_t usage) : cache(cache_), it(it_), cachedCoinUsage(usage)
{
    assert(!cache.hasModifier);
    cache.hasModifier = true;
}

CCoinsModifier::~CCoinsModifier()
{
    assert(cache.hasModifier);
    cache.hasModifier = false;
    it->second.coins.Cleanup();
    if ((it->second.flags & CCoinsCacheEntry::FRESH) && it->second.coins.IsSpent()) {
        cache.cacheCoins.erase(it);
    } else {
        // If the coin still exists after the modification, add the new usage
        cache.cachedCoinsUsage += it->second.coins.DynamicMemoryUsage();
    }
}

static const size_t MAX_OUTPUTS_PER_BLOCK = MAX_BLOCK_BASE_SIZE /  ::GetSerializeSize(CTxOut(), SER_NETWORK, PROTOCOL_VERSION); // TODO: merge with similar definition in undo.h.

const CCoins AccessByTxid(const CCoinsViewCache& view, const uint256& txid)
{
    COutPoint iter(txid, 0);
    while (iter.n < MAX_OUTPUTS_PER_BLOCK) {
        const CCoins& alternate = view.AccessCoin(iter);
        if (!alternate.IsSpent()) return alternate;
        ++iter.n;
    }
    return coinEmpty;
}
