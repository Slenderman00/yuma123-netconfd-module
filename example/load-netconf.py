#!/usr/bin/python

from lxml import etree
import time
import sys, os
import argparse
import tntapi
import yangrpc
from yangcli import yangcli

namespaces = {
    "nc": "urn:ietf:params:xml:ns:netconf:base:1.0",
    "nd": "urn:ietf:params:xml:ns:yang:ietf-network",
    "nt": "urn:ietf:params:xml:ns:yang:ietf-network-topology",
}

global args
args = None

parser = argparse.ArgumentParser()
parser.add_argument(
    "--config",
    help="Path to the netconf configuration *.xml file defining the configuration according to ietf-networks, ietf-networks-topology and netconf-node models e.g. ../networks.xml",
)
args = parser.parse_args()

tree = etree.parse(args.config)
network = tree.xpath("/nc:config/nd:networks/nd:network", namespaces=namespaces)[0]

conns = tntapi.network_connect(network)
yconns = tntapi.network_connect_yangrpc(network)

for node_name in yconns.keys():
    ok = yangcli(yconns[node_name], """delete /lsi-ivi-load:load""").xpath("./ok")
tntapi.network_commit(conns)


def sweep(resistance=5, step=-1, threshold=4.6):
    for node_name in yconns.keys():
        ok = yangcli(
            yconns[node_name],
            f"replace /lsi-ivi-load:load/channel[name='out2'] -- resistance={resistance}",
        ).xpath("./ok")
    tntapi.network_commit(conns)

    voltage = 9999
    while voltage > threshold:
        for node_name in yconns.keys():
            ok = yangcli(
                yconns[node_name],
                f"replace /lsi-ivi-load:load/channel[name='out2'] -- resistance={format(resistance, '.9f')}",
            ).xpath("./ok")
        tntapi.network_commit(conns)

        state = tntapi.network_get_state(
            network,
            conns,
            filter="""<filter type="xpath" select="/*[local-name()='load' or local-name()='load-state']/channel"/>""",
        )
        state_wo_ns = tntapi.strip_namespaces(state)
        for node_name in yconns.keys():
            voltage = state_wo_ns.xpath(
                "node[node-id='%s']/data/load-state/channel[name='%s']/measurement/voltage"
                % (node_name, "out2")
            )[0].text
            voltage = float(voltage)
            current = state_wo_ns.xpath(
                "node[node-id='%s']/data/load-state/channel[name='%s']/measurement/current"
                % (node_name, "out2")
            )[0].text
            current = float(current)

            print(f"# {resistance} Ohm, {voltage} v, {current} l")

        resistance = resistance + step

    return (resistance, voltage)


resistance, voltage = sweep(5, -0.05, 4.5)

for node_name in yconns.keys():
    ok = yangcli(yconns[node_name], """delete /lsi-ivi-load:load""").xpath("./ok")
tntapi.network_commit(conns)

print(f"# powersupply died at {resistance} ohm producing {voltage} volts.")


# for node_name in yconns.keys():
# 	voltage = yangcli(yconns[node_name], """xget /load-state/channel[name='out2']""").xpath('./measurement/voltage')
# 	print(voltage)
# tntapi.network_commit(conns)

# 	# Measure output current
# 	state = tntapi.network_get_state(network, conns, filter="""<filter type="xpath" select="/*[local-name()='outputs' or local-name()='outputs-state']/output"/>""")
# 	state_wo_ns=tntapi.strip_namespaces(state)
# 	for node_name in yconns.keys():
# 		current = state_wo_ns.xpath("node[node-id='%s']/data/outputs-state/output[name='%s']/measurement/current"%(node_name, "out1"))[0].text
# 		print("# %.3f %6.4f\n"%(voltage, float(current)))
#
# for node_name in yconns.keys():
# 	ok=yangcli(yconns[node_name], """delete /outputs""").xpath('./ok')
# tntapi.network_commit(conns)
#
