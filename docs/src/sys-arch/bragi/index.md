# bragi

> This part of the Handbook if Work-In-Progress.

bragi is the future interface definition language for use in Managarm.
bragi will replace protobuf for all the protocol files, this conversion is currently ongoing.

The code for bragi can be found at [https://github.com/managarm/bragi](https://github.com/managarm/bragi).

## Requirements
bragi requires `python3` and `lark-parser`.

## Sample code
```
enum enum1 {
	foo = 1,
	bar,
	baz
}

message msg1 1 {
head(256):
	byte[64] bab;
	byte[64] bib;

tail:
	optional tag(13) byte[3] bub = [1,2,3];
	optional byte[] beb = [7];
	string bob = "test";
}

message msg2 2 {
head(128):
	optional byte[64] bab;
	int32 bib = 2345;

tail:
	byte[4] bub = [1,2,3];
	byte beb = 7;
	byte bhb = 7;
	string bob = "test";
}

message msg3 3 {head(13): byte foo = 3;}

message msg4 4 {
head(64):
	uint64 foo;
	byte[16] arr;
tail:
	int32 baz;
	uint32[] bif;
}
```

### Compile the sample code
To compile the `sample.idl` sample code run:
```sh
python setup.py install
bragi
```
