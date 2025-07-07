#pragma once

namespace string
{

class layout
{
public:
    explicit layout();
    ~layout();

    auto container() -> layout&;
};

}