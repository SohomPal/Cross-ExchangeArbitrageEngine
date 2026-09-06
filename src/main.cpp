#include "core/fixed_point.hpp"

#include <iostream>

int main() {
    const auto price = core::parse_price("62143.27", 2);
    const auto quantity = core::parse_quantity("0.00125000", 8);
    std::cout << "Price ticks: " << price.raw()
              << "\nQuantity atoms: " << quantity.raw() << '\n';
}
