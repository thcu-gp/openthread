/*
 *  Copyright (c) 2022, The OpenThread Authors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the
 *     names of its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file
 *   This file includes implementation for the NAT64 translator.
 */

#include "nat64_translator.hpp"

#if OPENTHREAD_CONFIG_NAT64_TRANSLATOR_ENABLE

#include "instance/instance.hpp"

namespace ot {
namespace Nat64 {

RegisterLogModule("Nat64");

const char *StateToString(State aState)
{
    static const char *const kStateString[] = {
        "Disabled",
        "NotRunning",
        "Idle",
        "Active",
    };

    struct EnumCheck
    {
        InitEnumValidatorCounter();
        ValidateNextEnum(kStateDisabled);
        ValidateNextEnum(kStateNotRunning);
        ValidateNextEnum(kStateIdle);
        ValidateNextEnum(kStateActive);
    };

    return kStateString[aState];
}

Translator::Translator(Instance &aInstance)
    : InstanceLocator(aInstance)
    , mState(State::kStateDisabled)
    , mMappingExpirerTimer(aInstance)
{
    Random::NonCrypto::Fill(mNextMappingId);

    mNat64Prefix.Clear();
    mIp4Cidr.Clear();
    mMappingExpirerTimer.Start(kAddressMappingIdleTimeoutMsec);
}

Message *Translator::NewIp4Message(const Message::Settings &aSettings)
{
    Message *message = Get<Ip6::Ip6>().NewMessage(sizeof(Ip6::Header) - sizeof(Ip4::Header), aSettings);

    if (message != nullptr)
    {
        message->SetType(Message::kTypeIp4);
    }

    return message;
}

Error Translator::SendMessage(Message &aMessage)
{
    bool   freed  = false;
    Error  error  = kErrorDrop;
    Result result = TranslateToIp6(aMessage);

    VerifyOrExit(result == kForward);

    error = Get<Ip6::Ip6>().SendRaw(OwnedPtr<Message>(&aMessage).PassOwnership());
    freed = true;

exit:
    if (!freed)
    {
        aMessage.Free();
    }

    return error;
}

Translator::Result Translator::TranslateFromIp6(Message &aMessage)
{
    Result                res        = kDrop;
    ErrorCounters::Reason dropReason = ErrorCounters::kUnknown;
    Ip6::Headers          ip6Headers;
    Ip4::Header           ip4Header;
    uint16_t              srcPortOrId = 0;
    AddressMapping       *mapping     = nullptr;

    if (mIp4Cidr.mLength == 0 || !mNat64Prefix.IsValidNat64())
    {
        ExitNow(res = kNotTranslated);
    }

    // ParseFrom will do basic checks for the message, including the message length and IP protocol version.
    if (ip6Headers.ParseFrom(aMessage) != kErrorNone)
    {
        LogWarn("outgoing datagram is not a valid IPv6 datagram, drop");
        dropReason = ErrorCounters::Reason::kIllegalPacket;
        ExitNow(res = kDrop);
    }

    if (!ip6Headers.GetDestinationAddress().MatchesPrefix(mNat64Prefix))
    {
        ExitNow(res = kNotTranslated);
    }

    mapping = FindOrAllocateMapping(ip6Headers);
    if (mapping == nullptr)
    {
        LogWarn("failed to get a mapping for %s (mapping pool full?)",
                ip6Headers.GetSourceAddress().ToString().AsCString());
        dropReason = ErrorCounters::Reason::kNoMapping;
        ExitNow(res = kDrop);
    }

#if OPENTHREAD_CONFIG_NAT64_PORT_TRANSLATION_ENABLE
    srcPortOrId = mapping->mTranslatedPortOrId;
#else
    srcPortOrId           = ip6Headers.IsIcmp6() ? ip6Headers.GetIcmpHeader().GetId() : ip6Headers.GetSourcePort();
#endif

    aMessage.RemoveHeader(sizeof(Ip6::Header));

    ip4Header.Clear();
    ip4Header.InitVersionIhl();
    ip4Header.SetSource(mapping->mIp4);
    ip4Header.GetDestination().ExtractFromIp6Address(mNat64Prefix.mLength, ip6Headers.GetDestinationAddress());
    ip4Header.SetTtl(ip6Headers.GetIpHopLimit());
    ip4Header.SetIdentification(0);

    switch (ip6Headers.GetIpProto())
    {
    // The IP header is consumed , so the next header is at offset 0.
    case Ip6::kProtoUdp:
        ip4Header.SetProtocol(Ip4::kProtoUdp);
        ip6Headers.SetSourcePort(srcPortOrId);
        aMessage.Write(0, ip6Headers.GetUdpHeader());
        res = kForward;
        break;
    case Ip6::kProtoTcp:
        ip4Header.SetProtocol(Ip4::kProtoTcp);
        ip6Headers.SetSourcePort(srcPortOrId);
        aMessage.Write(0, ip6Headers.GetTcpHeader());
        res = kForward;
        break;
    case Ip6::kProtoIcmp6:
        ip4Header.SetProtocol(Ip4::kProtoIcmp);
        SuccessOrExit(TranslateIcmp6(aMessage, srcPortOrId));
        res = kForward;
        break;
    default:
        dropReason = ErrorCounters::Reason::kUnsupportedProto;
        ExitNow(res = kDrop);
    }

    // res here must be kForward based on the switch above.
    // TODO: Implement the logic for replying ICMP messages.
    ip4Header.SetTotalLength(sizeof(Ip4::Header) + aMessage.GetLength() - aMessage.GetOffset());
    Checksum::UpdateMessageChecksum(aMessage, ip4Header.GetSource(), ip4Header.GetDestination(),
                                    ip4Header.GetProtocol());
    Checksum::UpdateIp4HeaderChecksum(ip4Header);
    if (aMessage.Prepend(ip4Header) != kErrorNone)
    {
        // This should never happen since the IPv4 header is shorter than the IPv6 header.
        LogCrit("failed to prepend IPv4 head to translated message");
        ExitNow(res = kDrop);
    }
    aMessage.SetType(Message::kTypeIp4);
    mCounters.Count6To4Packet(ip6Headers.GetIpProto(), ip6Headers.GetIpLength());
    mapping->mCounters.Count6To4Packet(ip6Headers.GetIpProto(), ip6Headers.GetIpLength());

exit:
    if (res == Result::kDrop)
    {
        mErrorCounters.Count6To4(dropReason);
    }
    return res;
}

Translator::Result Translator::TranslateToIp6(Message &aMessage)
{
    Result                res        = Result::kDrop;
    ErrorCounters::Reason dropReason = ErrorCounters::kUnknown;
    Ip6::Header           ip6Header;
    Ip4::Headers          ip4Headers;
    uint16_t              dstPortOrId = 0;
    AddressMapping       *mapping     = nullptr;

    // Ip6::Header::ParseFrom may return an error value when the incoming message is an IPv4 datagram.
    // If the message is already an IPv6 datagram, forward it directly.
    VerifyOrExit(ip6Header.ParseFrom(aMessage) != kErrorNone, res = kNotTranslated);

    if (mIp4Cidr.mLength == 0)
    {
        // The NAT64 translation is bypassed (will be handled externally)
        LogWarn("incoming message is an IPv4 datagram but no IPv4 CIDR for NAT64 configured, drop");
        ExitNow(res = kForward);
    }

    if (!mNat64Prefix.IsValidNat64())
    {
        LogWarn("incoming message is an IPv4 datagram but no NAT64 prefix configured, drop");
        ExitNow(res = kDrop);
    }

    if (ip4Headers.ParseFrom(aMessage) != kErrorNone)
    {
        LogWarn("incoming message is neither IPv4 nor an IPv6 datagram, drop");
        dropReason = ErrorCounters::Reason::kIllegalPacket;
        ExitNow(res = kDrop);
    }

    mapping = FindMapping(ip4Headers);

    if (mapping == nullptr)
    {
        LogWarn("no mapping found for the IPv4 address");
        dropReason = ErrorCounters::Reason::kNoMapping;
        ExitNow(res = kDrop);
    }

#if OPENTHREAD_CONFIG_NAT64_PORT_TRANSLATION_ENABLE
    dstPortOrId = mapping->mSrcPortOrId;
#else
    dstPortOrId           = ip4Headers.IsIcmp4() ? ip4Headers.GetIcmpHeader().GetId() : ip4Headers.GetDestinationPort();
#endif

    aMessage.RemoveHeader(sizeof(Ip4::Header));

    ip6Header.Clear();
    ip6Header.InitVersionTrafficClassFlow();
    ip6Header.GetSource().SynthesizeFromIp4Address(mNat64Prefix, ip4Headers.GetSourceAddress());
    ip6Header.SetDestination(mapping->mIp6);
    ip6Header.SetFlow(0);
    ip6Header.SetHopLimit(ip4Headers.GetIpTtl());

    // Note: TCP and UDP are the same for both IPv4 and IPv6 except for the checksum calculation, we will update the
    // checksum in the payload later. However, we need to translate ICMPv6 messages to ICMP messages in IPv4.
    switch (ip4Headers.GetIpProto())
    {
    // The IP header is consumed , so the next header is at offset 0.
    case Ip4::kProtoUdp:
        ip6Header.SetNextHeader(Ip6::kProtoUdp);
        ip4Headers.SetDestinationPort(dstPortOrId);
        aMessage.Write(0, ip4Headers.GetUdpHeader());
        res = kForward;
        break;
    case Ip4::kProtoTcp:
        ip6Header.SetNextHeader(Ip6::kProtoTcp);
        ip4Headers.SetDestinationPort(dstPortOrId);
        aMessage.Write(0, ip4Headers.GetTcpHeader());
        res = kForward;
        break;
    case Ip4::kProtoIcmp:
        ip6Header.SetNextHeader(Ip6::kProtoIcmp6);
        SuccessOrExit(TranslateIcmp4(aMessage, dstPortOrId));
        res = kForward;
        break;
    default:
        dropReason = ErrorCounters::Reason::kUnsupportedProto;
        ExitNow(res = kDrop);
    }

    // res here must be kForward based on the switch above.
    // TODO: Implement the logic for replying ICMP datagrams.
    ip6Header.SetPayloadLength(aMessage.GetLength() - aMessage.GetOffset());
    Checksum::UpdateMessageChecksum(aMessage, ip6Header.GetSource(), ip6Header.GetDestination(),
                                    ip6Header.GetNextHeader());
    if (aMessage.Prepend(ip6Header) != kErrorNone)
    {
        // This might happen when the platform failed to reserve enough space before the original IPv4 datagram.
        LogWarn("failed to prepend IPv6 head to translated message");
        ExitNow(res = kDrop);
    }
    aMessage.SetType(Message::kTypeIp6);
    mCounters.Count4To6Packet(ip4Headers.GetIpProto(), ip4Headers.GetIpLength() - sizeof(Ip4::Header));
    mapping->mCounters.Count4To6Packet(ip4Headers.GetIpProto(), ip4Headers.GetIpLength() - sizeof(Ip4::Header));

exit:
    if (res == Result::kDrop)
    {
        mErrorCounters.Count4To6(dropReason);
    }

    return res;
}

Translator::AddressMapping::InfoString Translator::AddressMapping::ToString(void) const
{
    InfoString string;

    string.Append("%s -> %s", mIp6.ToString().AsCString(), mIp4.ToString().AsCString());

    return string;
}

void Translator::AddressMapping::CopyTo(otNat64AddressMapping &aMapping, TimeMilli aNow) const
{
    aMapping.mId                 = mId;
    aMapping.mIp4                = mIp4;
    aMapping.mIp6                = mIp6;
    aMapping.mSrcPortOrId        = mSrcPortOrId;
    aMapping.mTranslatedPortOrId = mTranslatedPortOrId;
    aMapping.mCounters           = mCounters;

    // We are removing expired mappings lazily, and an expired mapping might become active again before actually
    // removed. Report the mapping to be "just expired" to avoid confusion.
    if (mExpiry < aNow)
    {
        aMapping.mRemainingTimeMs = 0;
    }
    else
    {
        aMapping.mRemainingTimeMs = mExpiry - aNow;
    }
}

void Translator::ReleaseMapping(AddressMapping &aMapping)
{
    if (mIp4Cidr.mLength <= kMaxCidrLenForValidAddrPool)
    {
        // IPv4 addresses are allocated from the pool only when the pool size is above a minimum value.
        // Otherwise use just the first address from the list and we are not removing it from the array.
        IgnoreError(mIp4AddressPool.PushBack(aMapping.mIp4));
    }
    mAddressMappingPool.Free(aMapping);
    LogInfo("mapping removed: %s", aMapping.ToString().AsCString());
}

uint16_t Translator::ReleaseMappings(LinkedList<AddressMapping> &aMappings)
{
    uint16_t numRemoved = 0;

    for (AddressMapping *mapping = aMappings.Pop(); mapping != nullptr; mapping = aMappings.Pop())
    {
        numRemoved++;
        ReleaseMapping(*mapping);
    }

    return numRemoved;
}

uint16_t Translator::ReleaseExpiredMappings(void)
{
    LinkedList<AddressMapping> idleMappings;

    mActiveAddressMappings.RemoveAllMatching(idleMappings, TimerMilli::GetNow());

    return ReleaseMappings(idleMappings);
}
#if OPENTHREAD_CONFIG_NAT64_PORT_TRANSLATION_ENABLE
uint16_t Translator::AllocateSourcePort(uint16_t aSrcPort)
{
    // The translated port is randomly allocated from the range of dynamic or private ports (RFC 7605 section 4).
    // In this way, we will not pick a random port that could be a well-known port preventing an unknown situation on
    // the receiver side.
    uint16_t retPort;

    do
    {
        retPort = Random::NonCrypto::GetUint16InRange(kTranslationPortRangeStart, kTranslationPortRangeEnd);
        // The NAT64 SHOULD preserve the port parity (odd/even), as per Section 4.2.2 of [RFC4787]).
        // Determine if original and allocated port have different parity
        if (((aSrcPort ^ retPort) & 1) == 1)
        {
            retPort++;
        }
    } while (mActiveAddressMappings.ContainsMatching(retPort));

    return retPort;
}
#endif

Translator::AddressMapping *Translator::AllocateMapping(const Ip6::Headers &aIp6Headers)
{
    AddressMapping *mapping = nullptr;
    Ip4::Address    ip4Addr;

    // The NAT64 translator can work in 2 ways, either with a single IPv4 address or a larger pool of addresses. There
    // is also the corner case where the address pool is generated from a big CIDR length and the number of available
    // IPv4 addresses is not big enough to apply a 1 to 1 translation from IPv6 to IPv4 address. When operating in the
    // first case, there is no need to manage the address pool and all active mappings will use 1 single address (or the
    // limited number alternatively). If a larger pool is available each active mapping will use a separate IPv4
    // address.
    if (mIp4Cidr.mLength > kMaxCidrLenForValidAddrPool)
    {
        // TODO: add logic to cycle between available IPv4 addresses
        ip4Addr = *mIp4AddressPool.At(0);
    }
    else
    {
        if (mIp4AddressPool.IsEmpty())
        {
            // ReleaseExpiredMappings returns the number of mappings removed.
            VerifyOrExit(ReleaseExpiredMappings() > 0);
        }
        ip4Addr = *mIp4AddressPool.PopBack();
    }

    mapping = mAddressMappingPool.Allocate();
    // We should get a valid item, there is enough space in the mapping pool. Otherwise return null and fail the
    // translation.
    VerifyOrExit(mapping != nullptr);

    mActiveAddressMappings.Push(*mapping);
    mapping->mCounters.Clear();
    mapping->mId  = ++mNextMappingId;
    mapping->mIp6 = aIp6Headers.GetSourceAddress();
    mapping->mIp4 = ip4Addr;
#if OPENTHREAD_CONFIG_NAT64_PORT_TRANSLATION_ENABLE
    mapping->mSrcPortOrId = aIp6Headers.IsIcmp6() ? aIp6Headers.GetIcmpHeader().GetId() : aIp6Headers.GetSourcePort();
    // Allocate a unique source port or ICMP Id
    mapping->mTranslatedPortOrId = AllocateSourcePort(mapping->mSrcPortOrId);
#else
    mapping->mSrcPortOrId = 0;
    mapping->mTranslatedPortOrId = 0;
#endif
    mapping->Touch(TimerMilli::GetNow(), aIp6Headers.GetIpProto());
    LogInfo("mapping created: %s", mapping->ToString().AsCString());

exit:
    return mapping;
}

Translator::AddressMapping *Translator::FindOrAllocateMapping(const Ip6::Headers &aIp6Headers)
{
#if OPENTHREAD_CONFIG_NAT64_PORT_TRANSLATION_ENABLE
    uint16_t srcPortOrId    = aIp6Headers.IsIcmp6() ? aIp6Headers.GetIcmpHeader().GetId() : aIp6Headers.GetSourcePort();
    AddressMapping *mapping = mActiveAddressMappings.FindMatching(aIp6Headers.GetSourceAddress(), srcPortOrId);
#else
    AddressMapping *mapping      = mActiveAddressMappings.FindMatching(aIp6Headers.GetSourceAddress());
#endif

    // Exit if we found a valid mapping.
    VerifyOrExit(mapping == nullptr);

    mapping = AllocateMapping(aIp6Headers);

exit:
    return mapping;
}

Translator::AddressMapping *Translator::FindMapping(const Ip4::Headers &aIp4Headers)
{
    uint16_t dstPortOrId =
        aIp4Headers.IsIcmp4() ? aIp4Headers.GetIcmpHeader().GetId() : aIp4Headers.GetDestinationPort();

#if OPENTHREAD_CONFIG_NAT64_PORT_TRANSLATION_ENABLE
    AddressMapping *mapping = mActiveAddressMappings.FindMatching(aIp4Headers.GetDestinationAddress(), dstPortOrId);
#else
    AddressMapping *mapping      = mActiveAddressMappings.FindMatching(aIp4Headers.GetDestinationAddress());
    OT_UNUSED_VARIABLE(dstPortOrId);
#endif

    if (mapping != nullptr)
    {
        mapping->Touch(TimerMilli::GetNow(), aIp4Headers.GetIpProto());
    }
    return mapping;
}

void Translator::AddressMapping::Touch(TimeMilli aNow, uint8_t aProtocol)
{
    if ((aProtocol == Ip6::kProtoIcmp6) || (aProtocol == Ip4::kProtoIcmp))
    {
        mExpiry = aNow + kAddressMappingIcmpIdleTimeoutMsec;
    }
    else
    {
        mExpiry = aNow + kAddressMappingIdleTimeoutMsec;
    }
}

Error Translator::TranslateIcmp4(Message &aMessage, uint16_t aOriginalId)
{
    Error             err = kErrorNone;
    Ip4::Icmp::Header icmp4Header;
    Ip6::Icmp::Header icmp6Header;

    // TODO: Implement the translation of other ICMP messages.

    // Note: The caller consumed the IP header, so the ICMP header is at offset 0.
    SuccessOrExit(err = aMessage.Read(0, icmp4Header));
    switch (icmp4Header.GetType())
    {
    case Ip4::Icmp::Header::Type::kTypeEchoReply:
    {
        // The only difference between ICMPv6 echo and ICMP4 echo is the message type field, so we can reinterpret it as
        // ICMP6 header and set the message type.
        SuccessOrExit(err = aMessage.Read(0, icmp6Header));
        icmp6Header.SetType(Ip6::Icmp::Header::Type::kTypeEchoReply);
        icmp6Header.SetId(aOriginalId);
        aMessage.Write(0, icmp6Header);
        break;
    }
    default:
        err = kErrorInvalidArgs;
        break;
    }

exit:
    return err;
}

Error Translator::TranslateIcmp6(Message &aMessage, uint16_t aTranslatedId)
{
    Error             err = kErrorNone;
    Ip4::Icmp::Header icmp4Header;
    Ip6::Icmp::Header icmp6Header;

    // TODO: Implement the translation of other ICMP messages.

    // Note: The caller have consumed the IP header, so the ICMP header is at offset 0.
    SuccessOrExit(err = aMessage.Read(0, icmp6Header));
    switch (icmp6Header.GetType())
    {
    case Ip6::Icmp::Header::Type::kTypeEchoRequest:
    {
        // The only difference between ICMPv6 echo and ICMP4 echo is the message type field, so we can reinterpret it as
        // ICMP6 header and set the message type.
        SuccessOrExit(err = aMessage.Read(0, icmp4Header));
        icmp4Header.SetType(Ip4::Icmp::Header::Type::kTypeEchoRequest);
        icmp4Header.SetId(aTranslatedId);
        aMessage.Write(0, icmp4Header);
        break;
    }
    default:
        err = kErrorInvalidArgs;
        break;
    }

exit:
    return err;
}

Error Translator::SetIp4Cidr(const Ip4::Cidr &aCidr)
{
    Error err = kErrorNone;

    uint32_t numberOfHosts;
    uint32_t hostIdBegin;

    VerifyOrExit(aCidr.mLength > 0 && aCidr.mLength <= 32, err = kErrorInvalidArgs);

    VerifyOrExit(mIp4Cidr != aCidr);

    // Avoid using the 0s and 1s in the host id of an address, but what if the user provides us with /32 or /31
    // addresses?
    if (aCidr.mLength == 32)
    {
        hostIdBegin   = 0;
        numberOfHosts = 1;
    }
    else if (aCidr.mLength == 31)
    {
        hostIdBegin   = 0;
        numberOfHosts = 2;
    }
    else
    {
        hostIdBegin   = 1;
        numberOfHosts = static_cast<uint32_t>((1 << (Ip4::Address::kSize * 8 - aCidr.mLength)) - 2);
    }
    numberOfHosts = OT_MIN(numberOfHosts, kAddressMappingPoolSize);

    mAddressMappingPool.FreeAll();
    mActiveAddressMappings.Clear();
    mIp4AddressPool.Clear();

    for (uint32_t i = 0; i < numberOfHosts; i++)
    {
        Ip4::Address addr;

        addr.SynthesizeFromCidrAndHost(aCidr, i + hostIdBegin);
        IgnoreError(mIp4AddressPool.PushBack(addr));
    }

    LogInfo("IPv4 CIDR for NAT64: %s (actual address pool: %s - %s, %lu addresses)", aCidr.ToString().AsCString(),
            mIp4AddressPool.Front()->ToString().AsCString(), mIp4AddressPool.Back()->ToString().AsCString(),
            ToUlong(numberOfHosts));
    mIp4Cidr = aCidr;

    UpdateState();

    // Notify the platform when the CIDR is changed.
    Get<Notifier>().Signal(kEventNat64TranslatorStateChanged);

exit:
    return err;
}

void Translator::ClearIp4Cidr(void)
{
    mIp4Cidr.Clear();
    mAddressMappingPool.FreeAll();
    mActiveAddressMappings.Clear();
    mIp4AddressPool.Clear();

    UpdateState();
}

void Translator::SetNat64Prefix(const Ip6::Prefix &aNat64Prefix)
{
    if (aNat64Prefix.GetLength() == 0)
    {
        ClearNat64Prefix();
    }
    else if (mNat64Prefix != aNat64Prefix)
    {
        LogInfo("IPv6 Prefix for NAT64 updated to %s", aNat64Prefix.ToString().AsCString());
        mNat64Prefix = aNat64Prefix;
        UpdateState();
    }
}

void Translator::ClearNat64Prefix(void)
{
    VerifyOrExit(mNat64Prefix.GetLength() != 0);
    mNat64Prefix.Clear();
    LogInfo("IPv6 Prefix for NAT64 cleared");
    UpdateState();

exit:
    return;
}

void Translator::HandleMappingExpirerTimer(void)
{
    uint16_t numReleased = ReleaseExpiredMappings();

    LogInfo("Released %u expired mappings", numReleased);

    mMappingExpirerTimer.Start(Min(kAddressMappingIcmpIdleTimeoutMsec, kAddressMappingIdleTimeoutMsec));

    OT_UNUSED_VARIABLE(numReleased);
}

void Translator::InitAddressMappingIterator(AddressMappingIterator &aIterator)
{
    aIterator.mPtr = mActiveAddressMappings.GetHead();
}

Error Translator::GetNextAddressMapping(AddressMappingIterator &aIterator, otNat64AddressMapping &aMapping)
{
    Error           err  = kErrorNotFound;
    TimeMilli       now  = TimerMilli::GetNow();
    AddressMapping *item = static_cast<AddressMapping *>(aIterator.mPtr);

    VerifyOrExit(item != nullptr);

    item->CopyTo(aMapping, now);
    aIterator.mPtr = item->GetNext();
    err            = kErrorNone;

exit:
    return err;
}

Error Translator::GetIp4Cidr(Ip4::Cidr &aCidr)
{
    Error err = kErrorNone;

    VerifyOrExit(mIp4Cidr.mLength > 0, err = kErrorNotFound);
    aCidr = mIp4Cidr;

exit:
    return err;
}

Error Translator::GetIp6Prefix(Ip6::Prefix &aPrefix)
{
    Error err = kErrorNone;

    VerifyOrExit(mNat64Prefix.mLength > 0, err = kErrorNotFound);
    aPrefix = mNat64Prefix;

exit:
    return err;
}

void Translator::ProtocolCounters::Count6To4Packet(uint8_t aProtocol, uint64_t aPacketSize)
{
    switch (aProtocol)
    {
    case Ip6::kProtoUdp:
        mUdp.m6To4Packets++;
        mUdp.m6To4Bytes += aPacketSize;
        break;
    case Ip6::kProtoTcp:
        mTcp.m6To4Packets++;
        mTcp.m6To4Bytes += aPacketSize;
        break;
    case Ip6::kProtoIcmp6:
        mIcmp.m6To4Packets++;
        mIcmp.m6To4Bytes += aPacketSize;
        break;
    }

    mTotal.m6To4Packets++;
    mTotal.m6To4Bytes += aPacketSize;
}

void Translator::ProtocolCounters::Count4To6Packet(uint8_t aProtocol, uint64_t aPacketSize)
{
    switch (aProtocol)
    {
    case Ip4::kProtoUdp:
        mUdp.m4To6Packets++;
        mUdp.m4To6Bytes += aPacketSize;
        break;
    case Ip4::kProtoTcp:
        mTcp.m4To6Packets++;
        mTcp.m4To6Bytes += aPacketSize;
        break;
    case Ip4::kProtoIcmp:
        mIcmp.m4To6Packets++;
        mIcmp.m4To6Bytes += aPacketSize;
        break;
    }

    mTotal.m4To6Packets++;
    mTotal.m4To6Bytes += aPacketSize;
}

void Translator::UpdateState(void)
{
    State newState;

    if (mEnabled)
    {
        if (mIp4Cidr.mLength > 0 && mNat64Prefix.IsValidNat64())
        {
            newState = kStateActive;
        }
        else
        {
            newState = kStateNotRunning;
        }
    }
    else
    {
        newState = kStateDisabled;
    }

    SuccessOrExit(Get<Notifier>().Update(mState, newState, kEventNat64TranslatorStateChanged));
    LogInfo("NAT64 translator is now %s", StateToString(mState));

exit:
    return;
}

void Translator::SetEnabled(bool aEnabled)
{
    VerifyOrExit(mEnabled != aEnabled);
    mEnabled = aEnabled;

    if (!aEnabled)
    {
        ReleaseMappings(mActiveAddressMappings);
    }

    UpdateState();

exit:
    return;
}

} // namespace Nat64
} // namespace ot

#endif // OPENTHREAD_CONFIG_NAT64_TRANSLATOR_ENABLE
