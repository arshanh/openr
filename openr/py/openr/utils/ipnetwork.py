#!/usr/bin/env python3

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#


import ipaddress
import socket
from typing import List, Optional

from openr.Fib import ttypes as fib_types
from openr.Lsdb import ttypes as lsdb_types
from openr.Network import ttypes as network_types


def sprint_addr(addr):
    """ binary ip addr -> string """

    if not len(addr):
        return ""

    return str(ipaddress.ip_address(addr))


def sprint_prefix(prefix):
    """
    :param prefix: network_types.IpPrefix representing an CIDR network

    :returns: string representation of prefix (CIDR network)
    :rtype: str or unicode
    """

    return "{}/{}".format(sprint_addr(prefix.prefixAddress.addr), prefix.prefixLength)


def ip_str_to_addr(addr_str: str, if_index: str = None) -> network_types.BinaryAddress:
    """
    :param addr_str: ip address in string representation

    :returns: thrift struct BinaryAddress
    :rtype: network_types.BinaryAddress
    """

    # Try v4
    try:
        addr = socket.inet_pton(socket.AF_INET, addr_str)
        binary_address = network_types.BinaryAddress(addr=addr)
        if if_index:
            binary_address.ifName = if_index
        return binary_address
    except socket.error:
        pass

    # Try v6
    addr = socket.inet_pton(socket.AF_INET6, addr_str)
    binary_address = network_types.BinaryAddress(addr=addr)
    if if_index:
        binary_address.ifName = if_index
    return binary_address


def ip_str_to_prefix(prefix_str: str) -> network_types.IpPrefix:
    """
    :param prefix_str: string representing a prefix (CIDR network)

    :returns: thrift struct IpPrefix
    :rtype: network_types.IpPrefix
    """

    ip_str, ip_len_str = prefix_str.split("/")
    return network_types.IpPrefix(
        prefixAddress=ip_str_to_addr(ip_str), prefixLength=int(ip_len_str)
    )


def ip_nexthop_to_nexthop_thrift(
    ip_addr: str, if_index: str, weight: int = 0, metric: int = 0
) -> network_types.NextHopThrift:
    """
    :param ip_addr: Next hop IP address
    :param if_index: Next hop interface index
    :param weight: Next hop weigth
    :param metric: Cost associated with next hop
    """

    binary_address = ip_str_to_addr(ip_addr, if_index)
    return network_types.NextHopThrift(
        address=binary_address, weight=weight, metric=metric
    )


def ip_to_unicast_route(
    ip_prefix: str, nexthops: List[network_types.NextHopThrift]
) -> network_types.UnicastRoute:
    """
    :param ip_prefix: IP prefix
    :param nexthops: List of next hops
    """

    return network_types.UnicastRoute(
        dest=ip_str_to_prefix(ip_prefix), nextHops=nexthops
    )


def mpls_to_mpls_route(
    label: int, nexthops: List[network_types.NextHopThrift]
) -> network_types.UnicastRoute:
    """
    :param label: MPLS label
    :param nexthops: List of nexthops
    """

    return network_types.MplsRoute(topLabel=label, nextHops=nexthops)


def mpls_nexthop_to_nexthop_thrift(
    ip_addr: str,
    if_index: str,
    weight: int = 0,
    metric: int = 0,
    label: Optional[List[int]] = None,
    action: network_types.MplsActionCode = network_types.MplsActionCode.PHP,
) -> network_types.NextHopThrift:
    """
    :param label: label(s) for PUSH, SWAP action
    :param action: label action PUSH, POP, SWAP
    :param ip_addr: Next hop IP address
    :param if_index: Next hop interface index
    :param weight: Next hop weigth
    :param metric: Cost associated with next hop
    """

    binary_address = ip_str_to_addr(ip_addr, if_index)
    nexthop = network_types.NextHopThrift(
        address=binary_address, weight=weight, metric=metric
    )
    mpls_action = network_types.MplsAction(action=action)
    if action == network_types.MplsActionCode.SWAP:
        mpls_action.swapLabel = label[0]
    elif action == network_types.MplsActionCode.PUSH:
        mpls_action.pushLabels = label[:]

    nexthop.mplsAction = mpls_action
    return nexthop


def routes_to_route_db(
    node: str,
    unicast_routes: Optional[List[network_types.UnicastRoute]] = None,
    mpls_routes: Optional[List[network_types.MplsRoute]] = None,
) -> fib_types.RouteDatabase:
    """
    :param node: node name
    :param unicast_routes: list of unicast IP routes
    :param mpls_routes: list of MPLS routes
    """
    unicast_routes = [] if unicast_routes is None else unicast_routes
    mpls_routes = [] if mpls_routes is None else mpls_routes

    return fib_types.RouteDatabase(
        thisNodeName=node, unicastRoutes=unicast_routes, mplsRoutes=mpls_routes
    )


def sprint_prefix_type(prefix_type):
    """
    :param prefix: network_types.PrefixType
    """

    return network_types.PrefixType._VALUES_TO_NAMES.get(prefix_type, None)


def sprint_prefix_forwarding_type(forwarding_type):
    """
    :param forwarding_type: lsdb_types.PrefixForwardingType
    """

    return lsdb_types.PrefixForwardingType._VALUES_TO_NAMES.get(forwarding_type)


def sprint_prefix_forwarding_algorithm(
    forwarding_algo: lsdb_types.PrefixForwardingAlgorithm
) -> str:
    """
    :param forwarding_algorithm: lsdb_types.PrefixForwardingAlgorithm
    """

    return lsdb_types.PrefixForwardingAlgorithm._VALUES_TO_NAMES.get(forwarding_algo)


def sprint_prefix_is_ephemeral(prefix_entry: lsdb_types.PrefixEntry) -> str:
    """
    :param prefix_entry: lsdb_types.PrefixEntry
    """

    return (
        str(prefix_entry.ephemeral) if prefix_entry.ephemeral is not None else "False"
    )


def ip_version(addr):
    """ return ip addr version
    """

    return ipaddress.ip_address(addr).version


def is_same_subnet(addr1, addr2, subnet):
    """
    Check whether two given addresses belong to the same subnet
    """

    if ipaddress.ip_network((addr1, subnet), strict=False) == ipaddress.ip_network(
        (addr2, subnet), strict=False
    ):
        return True

    return False


def is_link_local(addr):
    """
    Check whether given addr is link local or not
    """
    return ipaddress.ip_network(addr).is_link_local


def is_subnet_of(a, b):
    """
    Check if network-b is subnet of network-a
    """

    if a.network_address != b.network_address:
        return False

    return a.prefixlen >= b.prefixlen


def contain_any_prefix(prefix, ip_networks):
    """
    Utility function to check if prefix contain any of the prefixes/ips

    :returns: True if prefix contains any of the ip_networks else False
    """

    if ip_networks is None:
        return True
    prefix = ipaddress.ip_network(prefix)
    return any(is_subnet_of(prefix, net) for net in ip_networks)
