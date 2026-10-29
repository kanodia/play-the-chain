"""Candid bytes for the ICP ledger calls in src/fuel.cpp.

The fuel tank uses two calls whose arguments are fixed for this canister:
  account_balance(record { account = <the canister's ICP account> })
  transfer(record { to = <the CMC's top-up account for the canister>;
                    memo = 0x50555054 ("TPUP"); fee; amount = <last 8 bytes> })
The accounts come from:
  dfx ledger account-id --of-canister ne5l7-xiaaa-aaaaj-qsm3q-cai
  dfx ledger account-id --of-canister rkp4c-7iaaa-aaaaa-aaaca-cai --subaccount <the canister's principal as a subaccount>

Run:  python3 -I scripts/fuel_args.py transfer 0      (hex; drop the last 8 bytes for the prefix)
      python3 -I scripts/fuel_args.py balance <account hex>
"""
import sys
def h(s):
    x = 0
    for c in s.encode(): x = (x * 223 + c) % 2**32
    return x
def leb(n):
    out = b""
    while True:
        b = n & 0x7f; n >>= 7
        if n: out += bytes([b | 0x80])
        else: return out + bytes([b])
def sleb(n):
    out = b""
    while True:
        b = n & 0x7f; n >>= 7
        if (n == 0 and not b & 0x40) or (n == -1 and b & 0x40): return out + bytes([b])
        out += bytes([b | 0x80])
NAT64, NAT8 = -8, -5  # Candid type codes: nat64 is -8 (0x78), nat8 -5 (0x7b)
def transfer(amount, to_hex, fee=10000, memo=0x50555054):
    # types: 0 = record{e8s: nat64}; 1 = vec nat8; 2 = opt 1;
    #        3 = record{timestamp_nanos: nat64}; 4 = opt 3; 5 = TransferArgs
    t = [sleb(-20) + leb(1) + leb(h("e8s")) + sleb(NAT64),
         sleb(-19) + sleb(NAT8),
         sleb(-18) + sleb(1),
         sleb(-20) + leb(1) + leb(h("timestamp_nanos")) + sleb(NAT64),
         sleb(-18) + sleb(3)]
    fields = {"memo": (sleb(NAT64), memo.to_bytes(8, "little")),
              "amount": (sleb(0), amount.to_bytes(8, "little")),
              "fee": (sleb(0), fee.to_bytes(8, "little")),
              "from_subaccount": (sleb(2), b"\x00"),
              "to": (sleb(1), leb(32) + bytes.fromhex(to_hex)),
              "created_at_time": (sleb(4), b"\x00")}
    order = sorted(fields, key=h)
    t.append(sleb(-20) + leb(len(order)) + b"".join(leb(h(f)) + fields[f][0] for f in order))
    return b"DIDL" + leb(len(t)) + b"".join(t) + leb(1) + sleb(5) + b"".join(fields[f][1] for f in order), order
def balance(acct_hex):
    t = [sleb(-19) + sleb(NAT8), sleb(-20) + leb(1) + leb(h("account")) + sleb(0)]
    return b"DIDL" + leb(2) + b"".join(t) + leb(1) + sleb(1) + leb(32) + bytes.fromhex(acct_hex)
CMC_TOPUP = "5136f1c0f5da7674c7958e7132ed7ef99d9bf8caad17b380fbd220b62e76a7ec"
if __name__ == "__main__":
    if sys.argv[1] == "transfer":
        b, order = transfer(int(sys.argv[2]), CMC_TOPUP); print(b.hex()); print(order, file=sys.stderr)
    else:
        print(balance(sys.argv[2]).hex())
