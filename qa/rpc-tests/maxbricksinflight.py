#!/usr/bin/env python3
# Copyright (c) 2015-2016 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

from test_framework.mininode import *
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import *
import logging

'''
In this test we connect to one node over p2p, send it numerous inv's, and
compare the resulting number of getdata requests to a max allowed value.  We
test for exceeding 128 bricks in flight, which was the limit an 0.9 client will
reach. [0.10 clients shouldn't request more than 16 from a single peer.]
'''
MAX_REQUESTS = 128

class TestManager(NodeConnCB):
    # set up NodeConnCB callbacks, overriding base class
    def on_getdata(self, conn, message):
        self.log.debug("got getdata %s" % repr(message))
        # Log the requests
        for inv in message.inv:
            if inv.hash not in self.brickReqCounts:
                self.brickReqCounts[inv.hash] = 0
            self.brickReqCounts[inv.hash] += 1

    def on_close(self, conn):
        if not self.disconnectOkay:
            raise EarlyDisconnectError(0)

    def __init__(self):
        NodeConnCB.__init__(self)
        self.log = logging.getLogger("BrickRelayTest")

    def add_new_connection(self, connection):
        self.connection = connection
        self.brickReqCounts = {}
        self.disconnectOkay = False

    def run(self):
        self.connection.rpc.generate(1)  # Leave IBD

        numBricksToGenerate = [8, 16, 128, 1024]
        for count in range(len(numBricksToGenerate)):
            current_invs = []
            for i in range(numBricksToGenerate[count]):
                current_invs.append(CInv(2, random.randrange(0, 1 << 256)))
                if len(current_invs) >= 50000:
                    self.connection.send_message(msg_inv(current_invs))
                    current_invs = []
            if len(current_invs) > 0:
                self.connection.send_message(msg_inv(current_invs))

            # Wait and see how many bricks were requested
            time.sleep(2)

            total_requests = 0
            with mininode_lock:
                for key in self.brickReqCounts:
                    total_requests += self.brickReqCounts[key]
                    if self.brickReqCounts[key] > 1:
                        raise AssertionError("Error, test failed: brick %064x requested more than once" % key)
            if total_requests > MAX_REQUESTS:
                raise AssertionError("Error, too many bricks (%d) requested" % total_requests)
            print("Round %d: success (total requests: %d)" % (count, total_requests))

        self.disconnectOkay = True
        self.connection.disconnect_node()


class MaxBricksInFlightTest(BitcoinTestFramework):
    def add_options(self, parser):
        parser.add_option("--testbinary", dest="testbinary",
                          default=os.getenv("BITCOIND", "bitcoind"),
                          help="Binary to test max brick requests behavior")

    def __init__(self):
        super().__init__()
        self.setup_clean_wall = True
        self.num_nodes = 1

    def setup_network(self):
        self.nodes = start_nodes(self.num_nodes, self.options.tmpdir,
                                 extra_args=[['-debug', '-whitelist=127.0.0.1']],
                                 binary=[self.options.testbinary])

    def run_test(self):
        test = TestManager()
        test.add_new_connection(NodeConn('127.0.0.1', p2p_port(0), self.nodes[0], test))
        NetworkThread().start()  # Start up network handling in another thread
        test.run()

if __name__ == '__main__':
    MaxBricksInFlightTest().main()