/*
 * ZeroTier One - Network Virtualization Everywhere
 * Copyright (C) 2011-2016  ZeroTier, Inc.  https://www.zerotier.com/
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef ZT_CAPABILITY_HPP
#define ZT_CAPABILITY_HPP

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Constants.hpp"
#include "Address.hpp"
#include "C25519.hpp"
#include "Utils.hpp"
#include "Buffer.hpp"
#include "Identity.hpp"
#include "../include/ZeroTierOne.h"

namespace ZeroTier {

class RuntimeEnvironment;

/**
 * A set of grouped and signed network flow rules
 *
 * The use of capabilities implements capability-based security on ZeroTIer
 * virtual networks for efficient and manageable network micro-segmentation.
 *
 * On the sending side the sender does the following for each packet:
 *
 * (1) Evaluates its capabilities in ascending order of ID to determine
 *     which capability allows it to transmit this packet.
 * (2) If it has not done so lately, it then sends this capability to the
 *     receving peer ("presents" it).
 * (3) The sender then sends the packet.
 *
 * On the receiving side the receiver does the following for each packet:
 *
 * (1) Evaluates the capabilities of the sender (that the sender has
 *     presented) to determine if the sender was allowed to send this.
 * (2) Evaluates its own capabilities to determine if it should receive
 *     and process this packet.
 * (3) If both check out, it receives the packet.
 *
 * Note that rules in capabilities can do other things as well such as TEE
 * or REDIRECT packets. See Filter and ZT_VirtualNetworkRule.
 */
class Capability
{
public:
	Capability()
	{
		memset(this,0,sizeof(Capability));
	}

	/**
	 * @param id Capability ID
	 * @param nwid Network ID
	 * @param expiration Expiration relative to network config timestamp
	 * @param name Capability short name (max strlen == ZT_MAX_CAPABILITY_NAME_LENGTH, overflow ignored)
	 * @param mccl Maximum custody chain length (1 to create non-transferrable capability)
	 * @param rules Network flow rules for this capability
	 * @param ruleCount Number of flow rules
	 */
	Capability(uint32_t id,uint64_t nwid,uint64_t expiration,const char *name,unsigned int mccl,const ZT_VirtualNetworkRule *rules,unsigned int ruleCount)
	{
		memset(this,0,sizeof(Capability));
		_nwid = nwid;
		_expiration = expiration;
		_id = id;
		_maxCustodyChainLength = (mccl > 0) ? ((mccl < ZT_MAX_CAPABILITY_CUSTODY_CHAIN_LENGTH) ? mccl : (unsigned int)ZT_MAX_CAPABILITY_CUSTODY_CHAIN_LENGTH) : 1;
		_ruleCount = (ruleCount < ZT_MAX_CAPABILITY_RULES) ? ruleCount : ZT_MAX_CAPABILITY_RULES;
		if (_ruleCount)
			memcpy(_rules,rules,sizeof(ZT_VirtualNetworkRule) * _ruleCount);
	}

	/**
	 * @return Rules -- see ruleCount() for size of array
	 */
	inline const ZT_VirtualNetworkRule *rules() const { return _rules; }

	/**
	 * @return Number of rules in rules()
	 */
	inline unsigned int ruleCount() const { return _ruleCount; }

	/**
	 * @return ID and evaluation order of this capability in network
	 */
	inline uint32_t id() const { return _id; }

	/**
	 * @return Network ID for which this capability was issued
	 */
	inline uint64_t networkId() const { return _nwid; }

	/**
	 * @return Expiration time relative to network config timestamp
	 */
	inline uint64_t expiration() const { return _expiration; }

	/**
	 * Sign this capability and add signature to its chain of custody
	 *
	 * If this returns false, this object should be considered to be
	 * in an undefined state and should be discarded. False can be returned
	 * if there is no more room for signatures (max chain length reached)
	 * or if the 'from' identity does not include a secret key to allow
	 * it to sign anything.
	 *
	 * @param from Signing identity (must have secret)
	 * @param to Recipient of this signature
	 * @return True if signature successful and chain of custody appended
	 */
	inline bool sign(const Identity &from,const Address &to)
	{
		try {
			for(unsigned int i=0;((i<_maxCustodyChainLength)&&(i<ZT_MAX_CAPABILITY_CUSTODY_CHAIN_LENGTH));++i) {
				if (!(_custody[i].to)) {
					_custody[i].to = to;
					_custody[i].from = from.address();
					Buffer<(sizeof(Capability) * 2)> tmp;
					this->serialize(tmp,true);
					_custody[i].signature = from.sign(tmp.data(),tmp.size());
					return true;
				}
			}
		} catch ( ... ) {}
		return false;
	}

	/**
	 * Verify this capability's chain of custody and signatures
	 *
	 * @param RR Runtime environment to provide for peer lookup, etc.
	 * @return 0 == OK, 1 == waiting for WHOIS, -1 == BAD signature or chain
	 */
	int verify(const RuntimeEnvironment *RR) const;

	template<unsigned int C>
	static inline void serializeRules(Buffer<C> &b,const ZT_VirtualNetworkRule *rules,unsigned int ruleCount)
	{
		b.append((uint16_t)ruleCount);
		for(unsigned int i=0;i<ruleCount;++i) {
			// Each rule consists of its 8-bit type followed by the size of that type's
			// field followed by field data. The inclusion of the size will allow non-supported
			// rules to be ignored but still parsed.
			b.append((uint8_t)rules[i].t);
			switch((ZT_VirtualNetworkRuleType)(rules[i].t & 0x7f)) {
				//case ZT_NETWORK_RULE_ACTION_DROP:
				//case ZT_NETWORK_RULE_ACTION_ACCEPT:
				default:
					b.append((uint8_t)0);
					break;
				case ZT_NETWORK_RULE_ACTION_TEE:
				case ZT_NETWORK_RULE_ACTION_REDIRECT:
				case ZT_NETWORK_RULE_MATCH_SOURCE_ZEROTIER_ADDRESS:
				case ZT_NETWORK_RULE_MATCH_DEST_ZEROTIER_ADDRESS:
					b.append((uint8_t)5);
					Address(rules[i].v.zt).appendTo(b);
					break;
				case ZT_NETWORK_RULE_MATCH_VLAN_ID:
					b.append((uint8_t)2);
					b.append((uint16_t)rules[i].v.vlanId);
					break;
				case ZT_NETWORK_RULE_MATCH_VLAN_PCP:
					b.append((uint8_t)1);
					b.append((uint8_t)rules[i].v.vlanPcp);
					break;
				case ZT_NETWORK_RULE_MATCH_VLAN_DEI:
					b.append((uint8_t)1);
					b.append((uint8_t)rules[i].v.vlanDei);
					break;
				case ZT_NETWORK_RULE_MATCH_ETHERTYPE:
					b.append((uint8_t)2);
					b.append((uint16_t)rules[i].v.etherType);
					break;
				case ZT_NETWORK_RULE_MATCH_MAC_SOURCE:
				case ZT_NETWORK_RULE_MATCH_MAC_DEST:
					b.append((uint8_t)6);
					b.append(rules[i].v.mac,6);
					break;
				case ZT_NETWORK_RULE_MATCH_IPV4_SOURCE:
				case ZT_NETWORK_RULE_MATCH_IPV4_DEST:
					b.append((uint8_t)5);
					b.append(&(rules[i].v.ipv4.ip),4);
					b.append((uint8_t)rules[i].v.ipv4.mask);
					break;
				case ZT_NETWORK_RULE_MATCH_IPV6_SOURCE:
				case ZT_NETWORK_RULE_MATCH_IPV6_DEST:
					b.append((uint8_t)17);
					b.append(rules[i].v.ipv6.ip,16);
					b.append((uint8_t)rules[i].v.ipv6.mask);
					break;
				case ZT_NETWORK_RULE_MATCH_IP_TOS:
					b.append((uint8_t)1);
					b.append((uint8_t)rules[i].v.ipTos);
					break;
				case ZT_NETWORK_RULE_MATCH_IP_PROTOCOL:
					b.append((uint8_t)1);
					b.append((uint8_t)rules[i].v.ipProtocol);
					break;
				case ZT_NETWORK_RULE_MATCH_IP_SOURCE_PORT_RANGE:
				case ZT_NETWORK_RULE_MATCH_IP_DEST_PORT_RANGE:
					b.append((uint8_t)4);
					b.append((uint16_t)rules[i].v.port[0]);
					b.append((uint16_t)rules[i].v.port[1]);
					break;
				case ZT_NETWORK_RULE_MATCH_CHARACTERISTICS:
					b.append((uint8_t)16);
					b.append((uint64_t)rules[i].v.characteristics[0]);
					b.append((uint64_t)rules[i].v.characteristics[1]);
					break;
				case ZT_NETWORK_RULE_MATCH_FRAME_SIZE_RANGE:
					b.append((uint8_t)4);
					b.append((uint16_t)rules[i].v.frameSize[0]);
					b.append((uint16_t)rules[i].v.frameSize[1]);
					break;
				case ZT_NETWORK_RULE_MATCH_TAG_VALUE_RANGE:
					b.append((uint8_t)12);
					b.append((uint32_t)rules[i].v.tag.id);
					b.append((uint32_t)rules[i].v.tag.value[0]);
					b.append((uint32_t)rules[i].v.tag.value[1]);
					break;
				case ZT_NETWORK_RULE_MATCH_TAG_VALUE_BITS_ALL:
				case ZT_NETWORK_RULE_MATCH_TAG_VALUE_BITS_ANY:
					b.append((uint8_t)8);
					b.append((uint32_t)rules[i].v.tag.id);
					b.append((uint32_t)rules[i].v.tag.value[0]);
					break;
			}
		}
	}

	template<unsigned int C>
	inline void serialize(Buffer<C> &b,const bool forSign = false) const
	{
		if (forSign) b.append((uint64_t)0x7f7f7f7f7f7f7f7fULL);

		b.append(_id);
		b.append(_nwid);
		b.append(_expiration);

		serializeRules(b,_rules,_ruleCount);

		b.append((uint8_t)_maxCustodyChainLength);
		for(unsigned int i=0;;++i) {
			if ((i < _maxCustodyChainLength)&&(i < ZT_MAX_CAPABILITY_CUSTODY_CHAIN_LENGTH)&&(_custody[i].to)) {
				_custody[i].to.appendTo(b);
				_custody[i].from.appendTo(b);
				if (!forSign) {
					b.append((uint8_t)1); // 1 == Ed25519 signature
					b.append((uint16_t)ZT_C25519_SIGNATURE_LEN); // length of signature
					b.append(_custody[i].signature.data,ZT_C25519_SIGNATURE_LEN);
				}
			} else {
				b.append((unsigned char)0,ZT_ADDRESS_LENGTH); // zero 'to' terminates chain
				break;
			}
		}

		// This is the size of any additional fields. If it is nonzero,
		// the last 2 bytes of the next field will be another size field.
		b.append((uint16_t)0);

		if (forSign) b.append((uint64_t)0x7f7f7f7f7f7f7f7fULL);
	}

	template<unsigned int C>
	static inline void deserializeRules(const Buffer<C> &b,unsigned int &p,ZT_VirtualNetworkRule *rules,unsigned int &ruleCount,const unsigned int maxRuleCount)
	{
		ruleCount = b.template at<uint16_t>(p); p += 2;
		if (ruleCount > maxRuleCount)
			throw std::runtime_error("rule count overflow");
		for(unsigned int i=0;i<ruleCount;++i) {
			rules[i].t = (uint8_t)b[p++];
			const unsigned int fieldLen = (unsigned int)b[p++];
			switch((ZT_VirtualNetworkRuleType)(rules[i].t & 0x7f)) {
				default:
					break;
				case ZT_NETWORK_RULE_ACTION_TEE:
				case ZT_NETWORK_RULE_ACTION_REDIRECT:
				case ZT_NETWORK_RULE_MATCH_SOURCE_ZEROTIER_ADDRESS:
				case ZT_NETWORK_RULE_MATCH_DEST_ZEROTIER_ADDRESS:
					rules[i].v.zt = Address(b.field(p,ZT_ADDRESS_LENGTH),ZT_ADDRESS_LENGTH).toInt();
					break;
				case ZT_NETWORK_RULE_MATCH_VLAN_ID:
					rules[i].v.vlanId = b.template at<uint16_t>(p);
					break;
				case ZT_NETWORK_RULE_MATCH_VLAN_PCP:
					rules[i].v.vlanPcp = (uint8_t)b[p];
					break;
				case ZT_NETWORK_RULE_MATCH_VLAN_DEI:
					rules[i].v.vlanDei = (uint8_t)b[p];
					break;
				case ZT_NETWORK_RULE_MATCH_ETHERTYPE:
					rules[i].v.etherType = b.template at<uint16_t>(p);
					break;
				case ZT_NETWORK_RULE_MATCH_MAC_SOURCE:
				case ZT_NETWORK_RULE_MATCH_MAC_DEST:
					memcpy(rules[i].v.mac,b.field(p,6),6);
					break;
				case ZT_NETWORK_RULE_MATCH_IPV4_SOURCE:
				case ZT_NETWORK_RULE_MATCH_IPV4_DEST:
					memcpy(&(rules[i].v.ipv4.ip),b.field(p,4),4);
					rules[i].v.ipv4.mask = (uint8_t)b[p + 4];
					break;
				case ZT_NETWORK_RULE_MATCH_IPV6_SOURCE:
				case ZT_NETWORK_RULE_MATCH_IPV6_DEST:
					memcpy(rules[i].v.ipv6.ip,b.field(p,16),16);
					rules[i].v.ipv6.mask = (uint8_t)b[p + 16];
					break;
				case ZT_NETWORK_RULE_MATCH_IP_TOS:
					rules[i].v.ipTos = (uint8_t)b[p];
					break;
				case ZT_NETWORK_RULE_MATCH_IP_PROTOCOL:
					rules[i].v.ipProtocol = (uint8_t)b[p];
					break;
				case ZT_NETWORK_RULE_MATCH_IP_SOURCE_PORT_RANGE:
				case ZT_NETWORK_RULE_MATCH_IP_DEST_PORT_RANGE:
					rules[i].v.port[0] = b.template at<uint16_t>(p);
					rules[i].v.port[1] = b.template at<uint16_t>(p + 2);
					break;
				case ZT_NETWORK_RULE_MATCH_CHARACTERISTICS:
					rules[i].v.characteristics[0] = b.template at<uint64_t>(p);
					rules[i].v.characteristics[1] = b.template at<uint64_t>(p + 8);
					break;
				case ZT_NETWORK_RULE_MATCH_FRAME_SIZE_RANGE:
					rules[i].v.frameSize[0] = b.template at<uint16_t>(p);
					rules[i].v.frameSize[0] = b.template at<uint16_t>(p + 2);
					break;
				case ZT_NETWORK_RULE_MATCH_TAG_VALUE_RANGE:
					rules[i].v.tag.id = b.template at<uint32_t>(p);
					rules[i].v.tag.value[0] = b.template at<uint32_t>(p + 4);
					rules[i].v.tag.value[1] = b.template at<uint32_t>(p + 8);
					break;
				case ZT_NETWORK_RULE_MATCH_TAG_VALUE_BITS_ALL:
				case ZT_NETWORK_RULE_MATCH_TAG_VALUE_BITS_ANY:
					rules[i].v.tag.id = b.template at<uint32_t>(p);
					rules[i].v.tag.value[0] = b.template at<uint32_t>(p + 4);
					break;
			}
			p += fieldLen;
		}
	}

	template<unsigned int C>
	inline unsigned int deserialize(const Buffer<C> &b,unsigned int startAt = 0)
	{
		memset(this,0,sizeof(Capability));

		unsigned int p = startAt;

		_id = b.template at<uint32_t>(p); p += 4;
		_nwid = b.template at<uint64_t>(p); p += 8;
		_expiration = b.template at<uint64_t>(p); p += 8;

		deserializeRules(b,p,_rules,_ruleCount,ZT_MAX_CAPABILITY_RULES);

		_maxCustodyChainLength = (unsigned int)b[p++];
		if ((_maxCustodyChainLength < 1)||(_maxCustodyChainLength > ZT_MAX_CAPABILITY_CUSTODY_CHAIN_LENGTH))
			throw std::runtime_error("invalid max custody chain length");
		for(unsigned int i;;++i) {
			const Address to(b.field(p,ZT_ADDRESS_LENGTH),ZT_ADDRESS_LENGTH); p += ZT_ADDRESS_LENGTH;
			if (!to)
				break;
			if ((i >= _maxCustodyChainLength)||(i >= ZT_MAX_CAPABILITY_CUSTODY_CHAIN_LENGTH))
				throw std::runtime_error("unterminated custody chain");
			_custody[i].to = to;
			_custody[i].from.setTo(b.field(p,ZT_ADDRESS_LENGTH),ZT_ADDRESS_LENGTH); p += ZT_ADDRESS_LENGTH;
			memcpy(_custody[i].signature.data,b.field(p,ZT_C25519_SIGNATURE_LEN),ZT_C25519_SIGNATURE_LEN); p += ZT_C25519_SIGNATURE_LEN;
		}

		p += 2 + b.template at<uint16_t>(p);
		if (p > b.size())
			throw std::runtime_error("extended field overflow");

		return (p - startAt);
	}

	/**
	 * Check to see if a given address is a 'to' address in the custody chain
	 *
	 * This does not actually do certificate checking. That must be done with verify().
	 *
	 * @param a Address to check
	 * @return True if address is present
	 */
	inline bool wasIssuedTo(const Address &a) const
	{
		for(unsigned int i=0;i<ZT_MAX_CAPABILITY_CUSTODY_CHAIN_LENGTH;++i) {
			if (!_custody[i].to)
				break;
			else if (_custody[i].to == a)
				return true;
		}
		return false;
	}

	// Provides natural sort order by ID
	inline bool operator<(const Capability &c) const { return (_id < c._id); }

	inline bool operator==(const Capability &c) const { return (memcmp(this,&c,sizeof(Capability)) == 0); }
	inline bool operator!=(const Capability &c) const { return (memcmp(this,&c,sizeof(Capability)) != 0); }

private:
	uint64_t _nwid;
	uint64_t _expiration;
	uint32_t _id;

	unsigned int _maxCustodyChainLength;

	unsigned int _ruleCount;
	ZT_VirtualNetworkRule _rules[ZT_MAX_CAPABILITY_RULES];

	struct {
		Address to;
		Address from;
		C25519::Signature signature;
	} _custody[ZT_MAX_CAPABILITY_CUSTODY_CHAIN_LENGTH];
};

} // namespace ZeroTier

#endif