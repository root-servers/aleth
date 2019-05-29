// Aleth: Ethereum C++ client, tools and libraries.
// Copyright 2019 Aleth Authors.
// Licensed under the GNU General Public License, Version 3.

#include "ENR.h"
#include <libdevcore/SHA3.h>

namespace dev
{
namespace p2p
{
namespace
{
constexpr char c_keyID[] = "id";
constexpr char c_keySecp256k1[] = "secp256k1";
constexpr char c_keyIP[] = "ip";
constexpr char c_keyTCP[] = "tcp";
constexpr char c_keyUDP[] = "udp";
constexpr char c_IDV4[] = "v4";
constexpr size_t c_ENRMaxSizeBytes = 300;


// Address can be either boost::asio::ip::address_v4 or boost::asio::ip::address_v6
template <class Address>
bytes addressToBytes(Address const& _address)
{
    auto const addressBytes = _address.to_bytes();
    return bytes(addressBytes.begin(), addressBytes.end());
}

template <std::size_t N>
std::array<byte, N> bytesToAddress(bytesConstRef _bytes)
{
    std::array<byte, N> address;
    std::copy_n(_bytes.begin(), N, address.begin());
    return address;
}
}  // namespace

ENR::ENR(RLP const& _rlp, VerifyFunction const& _verifyFunction)
{
    if (_rlp.data().size() > c_ENRMaxSizeBytes)
        BOOST_THROW_EXCEPTION(ENRIsTooBig());

    m_signature = _rlp[0].toBytes(RLP::VeryStrict);

    m_seq = _rlp[1].toInt<uint64_t>(RLP::VeryStrict);

    // read key-values into vector first, to check the order
    std::vector<std::pair<std::string const, bytes>> keyValuePairs;
    for (size_t i = 2; i < _rlp.itemCount(); i += 2)
    {
        auto const key = _rlp[i].toString(RLP::VeryStrict);
        auto const value = _rlp[i + 1].data().toBytes();
        keyValuePairs.push_back({key, value});
    }

    // transfer to map, this will order them
    m_keyValuePairs.insert(keyValuePairs.begin(), keyValuePairs.end());

    if (!std::equal(keyValuePairs.begin(), keyValuePairs.end(), m_keyValuePairs.begin()))
        BOOST_THROW_EXCEPTION(ENRKeysAreNotUniqueSorted());

    if (!_verifyFunction(m_keyValuePairs, dev::ref(m_signature), dev::ref(content())))
        BOOST_THROW_EXCEPTION(ENRSignatureIsInvalid());
}

ENR::ENR(uint64_t _seq, std::map<std::string, bytes> const& _keyValuePairs,
    SignFunction const& _signFunction)
  : m_seq{_seq}, m_keyValuePairs{_keyValuePairs}, m_signature{_signFunction(dev::ref(content()))}
{
}

bytes ENR::content() const
{
    RLPStream stream{contentRlpListItemCount()};
    streamContent(stream);
    return stream.out();
}


void ENR::streamRLP(RLPStream& _s) const
{
    _s.appendList(contentRlpListItemCount() + 1);
    _s << m_signature;
    streamContent(_s);
}

void ENR::streamContent(RLPStream& _s) const
{
    _s << m_seq;
    for (auto const& keyValue : m_keyValuePairs)
    {
        _s << keyValue.first;
        _s.appendRaw(keyValue.second);
    }
}

ENR ENR::update(
    std::map<std::string, bytes> const& _keyValuePairs, SignFunction const& _signFunction) const
{
    return ENR{m_seq + 1, _keyValuePairs, _signFunction};
}

std::string ENR::id() const
{
    auto itID = m_keyValuePairs.find(c_keyID);
    return itID == m_keyValuePairs.end() ? "" : RLP(itID->second).toString(RLP::VeryStrict);
}

boost::asio::ip::address ENR::ip() const
{
    auto itIP = m_keyValuePairs.find(c_keyIP);
    if (itIP == m_keyValuePairs.end())
        return {};

    auto rlpAddress = RLP{itIP->second};
    auto const addressBytes = rlpAddress.toBytesConstRef();

    if (rlpAddress.size() == 4)
        return ba::ip::address_v4{bytesToAddress<4>(addressBytes)};
    else if (rlpAddress.size() == 16)
        return ba::ip::address_v6{bytesToAddress<16>(addressBytes)};
    else
        BOOST_THROW_EXCEPTION(ENRUnsupportedIPAddress());
}

uint16_t ENR::tcpPort() const
{
    auto itTCP = m_keyValuePairs.find(c_keyTCP);
    return itTCP == m_keyValuePairs.end() ? 0 : RLP{itTCP->second}.toInt<uint16_t>(RLP::VeryStrict);
}

uint16_t ENR::udpPort() const
{
    auto itUDP = m_keyValuePairs.find(c_keyUDP);
    return itUDP == m_keyValuePairs.end() ? 0 : RLP{itUDP->second}.toInt<uint16_t>(RLP::VeryStrict);
}

ENR IdentitySchemeV4::createENR(Secret const& _secret, boost::asio::ip::address const& _ip,
    uint16_t _tcpPort, uint16_t _udpPort)
{
    ENR::SignFunction signFunction = [&_secret](
                                         bytesConstRef _data) { return sign(_data, _secret); };

    auto const keyValuePairs = createKeyValuePairs(_secret, _ip, _tcpPort, _udpPort);

    return ENR{1 /* sequence number */, keyValuePairs, signFunction};
}

bytes IdentitySchemeV4::sign(bytesConstRef _data, Secret const& _secret)
{
    // dev::sign returns 65 bytes signature containing r,s,v values
    Signature s = dev::sign(_secret, sha3(_data));
    // The resulting 64-byte signature is encoded as the concatenation of the r and s signature
    // values.
    return bytes(&s[0], &s[64]);
}

std::map<std::string, bytes> IdentitySchemeV4::createKeyValuePairs(Secret const& _secret,
    boost::asio::ip::address const& _ip, uint16_t _tcpPort, uint16_t _udpPort)
{
    PublicCompressed const publicKey = toPublicCompressed(_secret);

    auto const address = _ip.is_v4() ? addressToBytes(_ip.to_v4()) : addressToBytes(_ip.to_v6());

    // Values are of different types (string, bytes, uint16_t),
    // so we store them as RLP representation
    return {{c_keyID, rlp(c_IDV4)}, {c_keySecp256k1, rlp(publicKey.asBytes())},
        {c_keyIP, rlp(address)}, {c_keyTCP, rlp(_tcpPort)}, {c_keyUDP, rlp(_udpPort)}};
}

ENR IdentitySchemeV4::updateENR(ENR const& _enr, Secret const& _secret,
    boost::asio::ip::address const& _ip, uint16_t _tcpPort, uint16_t _udpPort)
{
    ENR::SignFunction signFunction = [&_secret](
                                         bytesConstRef _data) { return sign(_data, _secret); };

    auto const keyValuePairs = createKeyValuePairs(_secret, _ip, _tcpPort, _udpPort);

    return _enr.update(keyValuePairs, signFunction);
}

ENR IdentitySchemeV4::parseENR(RLP const& _rlp)
{
    ENR::VerifyFunction verifyFunction = [](std::map<std::string, bytes> const& _keyValuePairs,
                                             bytesConstRef _signature, bytesConstRef _data) {
        auto itID = _keyValuePairs.find(c_keyID);
        if (itID == _keyValuePairs.end())
            return false;
        auto const id = RLP(itID->second).toString(RLP::VeryStrict);
        if (id != c_IDV4)
            return false;

        auto itKey = _keyValuePairs.find(c_keySecp256k1);
        if (itKey == _keyValuePairs.end())
            return false;

        auto const key = RLP(itKey->second).toHash<PublicCompressed>(RLP::VeryStrict);
        h512 const signature{_signature};

        return dev::verify(key, signature, sha3(_data));
    };

    return ENR{_rlp, verifyFunction};
}

PublicCompressed IdentitySchemeV4::publicKey(ENR const& _enr)
{
    auto const& keyValuePairs = _enr.keyValuePairs();

    auto itID = keyValuePairs.find(c_keyID);
    if (itID == keyValuePairs.end() || RLP(itID->second).toString(RLP::VeryStrict) != c_IDV4)
        BOOST_THROW_EXCEPTION(ENRUnknownIdentityScheme());

    auto itKey = keyValuePairs.find(c_keySecp256k1);
    if (itKey == keyValuePairs.end())
        BOOST_THROW_EXCEPTION(ENRSecp256k1NotFound());

    return RLP{itKey->second}.toHash<PublicCompressed>();
}

std::ostream& operator<<(std::ostream& _out, ENR const& _enr)
{
    _out << "[ seq=" << _enr.sequenceNumber() << " "
         << "id=" << _enr.id() << " ";

    try
    {
        auto const pubKey = IdentitySchemeV4::publicKey(_enr);
        auto const address = _enr.ip();
        auto const tcp = _enr.tcpPort();
        auto const udp = _enr.udpPort();

        _out << "key=" << pubKey.abridged() << " ip=" << address << " tcp=" << tcp
             << " udp=" << udp;
    }
    catch (Exception const&)
    {
        // If failed to get V4 fields, just dump all values
        for (auto const& keyValue : _enr.keyValuePairs())
        {
            _out << keyValue.first << "=";
            _out << toHexPrefixed(RLP{keyValue.second}.toBytes()) << " ";
        }
    }

    _out << " ]";
    return _out;
}

}  // namespace p2p
}  // namespace dev