{
    "targets": [{
        "target_name": "native_fs",
        "sources": [ "main.cc" ],
        "include_dirs" : [
            "<!(node -e \"require('nan')\")"
        ]
    }]
}
