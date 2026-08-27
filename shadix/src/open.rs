use libc::O_SYNC;
use std::{
    fs::{File, OpenOptions},
    io,
    os::unix::fs::OpenOptionsExt,
    path::Path,
};

pub unsafe fn open(path: impl AsRef<Path>) -> Result<File, io::Error> {
    OpenOptions::new()
        .write(true)
        .read(true)
        .custom_flags(O_SYNC)
        .open(path)
}
