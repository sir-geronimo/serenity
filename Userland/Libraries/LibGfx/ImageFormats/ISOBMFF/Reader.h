/*
 * Copyright (c) 2023, Gregory Bertilson <Zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/MaybeOwned.h>
#include <AK/Stream.h>

#include "Boxes.h"

namespace Gfx::ISOBMFF {

class Reader {
public:
    static ErrorOr<Reader> create(MaybeOwned<SeekableStream> stream);
    static ErrorOr<Reader> create(MaybeOwned<ConstrainedStream> stream);

    ErrorOr<BoxList> read_entire_file();

    using BoxCallback = Function<ErrorOr<Optional<NonnullOwnPtr<Box>>>(BoxType, ConstrainedStream&)>;
    ErrorOr<BoxList> read_entire_file(BoxCallback);

private:
    Reader(MaybeOwned<ConstrainedStream> stream)
        : m_box_stream(move(stream))
    {
    }

    MaybeOwned<ConstrainedStream> m_box_stream;
};

}
