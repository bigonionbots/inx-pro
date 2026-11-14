#pragma once

/**
 * @file DnsFallback.h
 * @brief Public DNS servers behind the ones Wi-Fi networks hand out.
 *
 * Some networks give the T5 no DNS server, or one that does not answer it: phone hotspots in
 * particular relay DNS for the phone's own apps but not for IPv4 clients, so a public name
 * (an Audiobookshelf or OPDS server) came back "not found" while the phone itself resolved it.
 *
 * On every connection (got an IP) this fills the DNS servers the network left empty with
 * 1.1.1.1 and sets 8.8.8.8 as the fallback slot, which DHCP does not overwrite. lwIP asks the
 * servers in order and moves on when one does not answer, so a network's own DNS still comes
 * first (local names keep working) and the public ones only answer when it is silent.
 */
namespace DnsFallback {

/** Registers the Wi-Fi event handler. Call once at boot, before any Wi-Fi use. */
void begin();

}  // namespace DnsFallback
