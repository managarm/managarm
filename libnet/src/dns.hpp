
#ifndef LIBNET_DNS_HPP
#define LIBNET_DNS_HPP

#include <string>

namespace libnet {

struct DnsHeader {
	uint16_t identification;
	uint16_t flags;
	uint16_t totalQuestions;
	uint16_t totalAnswerRRs;
	uint16_t totalAuthorityRRs;
	uint16_t totalAdditionalRRs;
};

} // namespace libnet

#endif // LIBNET_DNS_HPP



