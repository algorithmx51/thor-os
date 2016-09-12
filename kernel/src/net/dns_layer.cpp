//=======================================================================
// Copyright Baptiste Wicht 2013-2016.
// Distributed under the terms of the MIT License.
// (See accompanying file LICENSE or copy at
//  http://www.opensource.org/licenses/MIT)
//=======================================================================

#include <bit_field.hpp>

#include "net/dns_layer.hpp"
#include "net/udp_layer.hpp"

#include "kernel_utils.hpp"

namespace {

using flag_qr     = std::bit_field<uint16_t, uint8_t, 0, 1>;
using flag_opcode = std::bit_field<uint16_t, uint8_t, 1, 4>;
using flag_aa     = std::bit_field<uint16_t, uint8_t, 5, 1>;
using flag_tc     = std::bit_field<uint16_t, uint8_t, 6, 1>;
using flag_rd     = std::bit_field<uint16_t, uint8_t, 7, 1>;
using flag_ra     = std::bit_field<uint16_t, uint8_t, 8, 1>;
using flag_zeroes = std::bit_field<uint16_t, uint8_t, 9, 3>;
using flag_rcode  = std::bit_field<uint16_t, uint8_t, 12, 4>;

void prepare_packet_query(network::ethernet::packet& packet, uint16_t identification) {
    packet.tag(3, packet.index);

    // Set the DNS header

    auto* dns_header = reinterpret_cast<network::dns::header*>(packet.payload + packet.index);

    // Set the identification
    dns_header->identification = switch_endian_16(identification);

    // There is one question, nothing else
    dns_header->questions      = switch_endian_16(1);
    dns_header->answers        = switch_endian_16(0);
    dns_header->authority_rrs  = switch_endian_16(0);
    dns_header->additional_rrs = switch_endian_16(0);

    // Set all the flags
    flag_qr(&dns_header->flags) = 0;     // This is a query
    flag_opcode(&dns_header->flags) = 0; // This is a standard query
    flag_aa(&dns_header->flags) = 0;     // This is a query (field not used)
    flag_tc(&dns_header->flags) = 0;     // The question is not truncated
    flag_rd(&dns_header->flags) = 0;     // No need for recursion
    flag_ra(&dns_header->flags) = 0;     // This is a query (field not used)
    flag_zeroes(&dns_header->flags) = 0; // Always zero
    flag_rcode(&dns_header->flags) = 0;  // This is a query (field not used)

    packet.index += sizeof(network::dns::header);
}

std::string decode_domain(char* payload, size_t& offset) {
    std::string domain;

    offset = 0;

    while (true) {
        auto label_size = static_cast<uint8_t>(*(payload + offset));
        ++offset;

        if (!label_size) {
            break;
        }

        if (!domain.empty()) {
            domain += '.';
        }

        for (size_t i = 0; i < label_size; ++i) {
            domain += *(payload + offset);
            ++offset;
        }
    }

    return domain;
}

} //end of anonymous namespace

void network::dns::decode(network::interface_descriptor& /*interface*/, network::ethernet::packet& packet) {
    packet.tag(3, packet.index);

    auto* dns_header = reinterpret_cast<header*>(packet.payload + packet.index);

    logging::logf(logging::log_level::TRACE, "dns: Start DNS packet handling\n");

    auto identification = switch_endian_16(dns_header->identification);
    auto questions      = switch_endian_16(dns_header->questions);
    auto answers        = switch_endian_16(dns_header->answers);
    auto authority_rrs  = switch_endian_16(dns_header->authority_rrs);
    auto additional_rrs = switch_endian_16(dns_header->additional_rrs);

    logging::logf(logging::log_level::TRACE, "dns: Identification %h \n", size_t(identification));
    logging::logf(logging::log_level::TRACE, "dns: Answers %u \n", size_t(answers));
    logging::logf(logging::log_level::TRACE, "dns: Questions %u \n", size_t(questions));
    logging::logf(logging::log_level::TRACE, "dns: Authorithy RRs %u \n", size_t(authority_rrs));
    logging::logf(logging::log_level::TRACE, "dns: Additional RRs %u \n", size_t(additional_rrs));

    if (*flag_qr(&dns_header->flags)) {
        logging::logf(logging::log_level::TRACE, "dns: Query\n");
    } else {
        auto response_code = *flag_opcode(&dns_header->flags);

        if (response_code == 0x0) {
            logging::logf(logging::log_level::TRACE, "dns: Response OK\n");

            auto* payload = packet.payload + packet.index + sizeof(header);

            // Decode the questions (simply wrap around it)

            for (size_t i = 0; i < questions; ++i) {
                size_t length;
                auto domain = decode_domain(payload, length);

                payload += length;

                auto rr_type = switch_endian_16(*reinterpret_cast<uint16_t*>(payload));
                payload += 2;

                auto rr_class = switch_endian_16(*reinterpret_cast<uint16_t*>(payload));
                payload += 2;

                logging::logf(logging::log_level::TRACE, "dns: Query %u Type %u Class %u Name %s\n", i, rr_type, rr_class, domain.c_str());
            }

            for (size_t i = 0; i < answers; ++i) {
                auto label = static_cast<uint8_t>(*payload);

                std::string domain;
                if (label > 64) {
                    // This is a pointer
                    auto pointer = switch_endian_16(*reinterpret_cast<uint16_t*>(payload));
                    auto offset  = pointer & (0xFFFF >> 2);

                    payload += 2;

                    size_t ignored;
                    domain = decode_domain(packet.payload + packet.index + offset, ignored);
                } else {
                    logging::logf(logging::log_level::TRACE, "dns: Unable to handle non-compressed data\n");
                    return;
                }

                auto rr_type = switch_endian_16(*reinterpret_cast<uint16_t*>(payload));
                payload += 2;

                auto rr_class = switch_endian_16(*reinterpret_cast<uint16_t*>(payload));
                payload += 2;

                auto ttl = switch_endian_32(*reinterpret_cast<uint32_t*>(payload));
                payload += 4;

                auto rd_length = switch_endian_16(*reinterpret_cast<uint16_t*>(payload));
                payload += 2;

                if (rr_type == 0x1 && rr_class == 0x1) {
                    auto ip     = network::ip::ip32_to_ip(*reinterpret_cast<uint32_t*>(payload));
                    auto ip_str = network::ip::ip_to_str(ip);

                    logging::logf(logging::log_level::TRACE, "dns: Answer %u Domain %s Type %u Class %u TTL %u IP: %s\n", i, domain.c_str(), rr_type, rr_class, ttl, ip_str.c_str());
                } else {
                    logging::logf(logging::log_level::TRACE, "dns: Answer %u Domain %s Type %u Class %u TTL %u \n", i, rr_type, rr_class, ttl, domain.c_str());
                    logging::logf(logging::log_level::TRACE, "dns: Answer %u Unable to read data for type and class\n", i);
                }

                payload += rd_length;
            }
        } else if (response_code == 0x1) {
            logging::logf(logging::log_level::TRACE, "dns: Format Error\n");
        } else if (response_code == 0x2) {
            logging::logf(logging::log_level::TRACE, "dns: Server Failure\n");
        } else if (response_code == 0x3) {
            logging::logf(logging::log_level::TRACE, "dns: Name Error\n");
        } else if (response_code == 0x4) {
            logging::logf(logging::log_level::TRACE, "dns: Not Implemented\n");
        } else if (response_code == 0x5) {
            logging::logf(logging::log_level::TRACE, "dns: Refused\n");
        }
    }

    network::propagate_packet(packet, network::socket_protocol::DNS);
}

std::expected<network::ethernet::packet> network::dns::prepare_packet_query(network::interface_descriptor& interface, network::ip::address target_ip, uint16_t source_port, uint16_t identification, size_t payload_size) {
    // Ask the UDP layer to craft a packet
    auto packet = network::udp::prepare_packet(interface, target_ip, source_port, 53, sizeof(header) + payload_size);

    if (packet) {
        ::prepare_packet_query(*packet, identification);
    }

    return packet;
}

std::expected<network::ethernet::packet> network::dns::prepare_packet_query(char* buffer, network::interface_descriptor& interface, network::ip::address target_ip, uint16_t source_port, uint16_t identification, size_t payload_size) {
    // Ask the UDP layer to craft a packet
    auto packet = network::udp::prepare_packet(buffer, interface, target_ip, source_port, 53, sizeof(header) + payload_size);

    if (packet) {
        ::prepare_packet_query(*packet, identification);
    }

    return packet;
}

void network::dns::finalize_packet(network::interface_descriptor& interface, network::ethernet::packet& p) {
    p.index -= sizeof(header);

    // Give the packet to the UDP layer for finalization
    network::udp::finalize_packet(interface, p);
}
