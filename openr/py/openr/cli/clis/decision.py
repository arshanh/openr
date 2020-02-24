#!/usr/bin/env python3

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#


import sys
from builtins import object
from typing import Any, List

import click
from openr.cli.commands import decision
from openr.cli.utils.utils import parse_nodes


class DecisionCli(object):
    def __init__(self):
        self.decision.add_command(PathCli().path)
        self.decision.add_command(DecisionAdjCli().adj)
        self.decision.add_command(DecisionPrefixesCli().prefixes)
        self.decision.add_command(
            DecisionRoutesComputedCli().routes, name="routes-computed"
        )
        self.decision.add_command(
            DecisionRoutesUnInstallableCli().routes, name="routes-uninstallable"
        )

        # for TG backward compatibility. Deprecated.
        self.decision.add_command(DecisionRoutesComputedCli().routes, name="routes")
        self.decision.add_command(DecisionValidateCli().validate)

    @click.group()
    @click.pass_context
    def decision(ctx):  # noqa: B902
        """ CLI tool to peek into Decision module. """
        pass


class PathCli(object):
    @click.command()
    @click.option(
        "--src", default="", help="source node, " "default will be the current host"
    )
    @click.option(
        "--dst",
        default="",
        help="destination node or prefix, " "default will be the current host",
    )
    @click.option("--max-hop", default=256, help="max hop count")
    @click.option("--area", default=None, help="area identifier")
    @click.pass_obj
    def path(cli_opts, src, dst, max_hop, area):  # noqa: B902
        """ path from src to dst """

        decision.PathCmd(cli_opts).run(src, dst, max_hop, area)


class DecisionRoutesComputedCli(object):
    @click.command()
    @click.option(
        "--nodes",
        default="",
        help="Get routes for a list of nodes. Default will get "
        "host's routes. Get routes for all nodes if 'all' is given.",
    )
    @click.option(
        "--prefixes",
        "-p",
        default="",
        multiple=True,
        help="Get route for specific IPs or Prefixes.",
    )
    @click.option(
        "--labels",
        "-l",
        type=click.INT,
        multiple=True,
        help="Get route for specific labels.",
    )
    @click.option("--json/--no-json", default=False, help="Dump in JSON format")
    @click.pass_obj
    def routes(cli_opts, nodes, prefixes, labels, json):  # noqa: B902
        """ Request the routing table from Decision module """

        nodes = parse_nodes(cli_opts, nodes)
        decision.DecisionRoutesComputedCmd(cli_opts).run(nodes, prefixes, labels, json)


class DecisionRoutesUnInstallableCli(object):
    @click.command()
    @click.option(
        "--prefixes",
        "-p",
        default="",
        multiple=True,
        help="Get route for specific IPs or Prefixes.",
    )
    @click.option(
        "--labels",
        "-l",
        type=click.INT,
        multiple=True,
        help="Get route for specific labels.",
    )
    @click.option("--json/--no-json", default=False, help="Dump in JSON format")
    @click.pass_obj
    def routes(cli_opts, prefixes, labels, json):  # noqa: B902
        """ Request un installable routing table of the current host """

        decision.DecisionRoutesUnInstallableCmd(cli_opts).run(prefixes, labels, json)


class DecisionPrefixesCli(object):
    @click.command()
    @click.option(
        "--nodes",
        default="",
        help="Dump prefixes for a list of nodes. Default will dump host's "
        "prefixes. Dump prefixes for all nodes if 'all' is given.",
    )
    @click.option("--json/--no-json", default=False, help="Dump in JSON format")
    @click.option("--prefix", "-p", default="", help="Prefix filter. Exact match")
    @click.option(
        "--client-type",
        "-c",
        default="",
        help="Client type filter. Provide name e.g. loopback, bgp",
    )
    @click.pass_obj
    def prefixes(
        cli_opts: Any,  # noqa: B902
        nodes: List[str],
        json: bool,
        prefix: str,
        client_type: str,
    ) -> None:
        """ show the prefixes from Decision module """

        nodes = parse_nodes(cli_opts, nodes)
        decision.DecisionPrefixesCmd(cli_opts).run(nodes, json, prefix, client_type)


class DecisionAdjCli(object):
    @click.command()
    @click.option(
        "--nodes",
        default="",
        help="Dump adjacencies for a list of nodes. Default will dump "
        "host's adjs. Dump adjs for all nodes if 'all' is given",
    )
    @click.option("--bidir/--no-bidir", default=True, help="Only bidir adjacencies")
    @click.option("--json/--no-json", default=False, help="Dump in JSON format")
    @click.pass_obj
    def adj(cli_opts, nodes, bidir, json):  # noqa: B902
        """ dump the link-state adjacencies from Decision module """

        nodes = parse_nodes(cli_opts, nodes)
        decision.DecisionAdjCmd(cli_opts).run(nodes, bidir, json)


class DecisionValidateCli(object):
    @click.command()
    @click.option("--json/--no-json", default=False, help="Dump in JSON format")
    @click.option("--area", default=None, help="area identifier")
    @click.pass_obj
    def validate(cli_opts, json, area):  # noqa: B902
        """
        Check all prefix & adj dbs in Decision against that in KvStore

        If --json is provided, returns database diffs in the following format.
        "neighbor_down" is a list of nodes not in the inspected node's dump that were expected,
        "neighbor_up" is a list of unexpected nodes in inspected node's dump,
        "neighbor_update" is a list of expected nodes whose metadata are unexpected.
            {
                "neighbor_down": [
                    {
                        "new_adj": null,
                        "old_adj": $inconsistent_node
                    }
                ],
                "neighbor_up": [
                    {
                        "new_adj": $inconsistent_node
                        "old_adj": null
                    }
                ],
                "neighbor_update": [
                    {
                        "new_adj": $inconsistent_node
                        "old_adj": $inconsistent_node
                    }
                ]
            }
        """

        return_code = decision.DecisionValidateCmd(cli_opts).run(json, area)
        sys.exit(return_code)
