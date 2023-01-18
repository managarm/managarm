# Coding standard for The Managarm Project

The Managarm Project follows the following coding standard.

## Managarm-specific Guidelines

This section lists the most important coding style guidelines that Managarm follows. Specifically, we list the guidelines for which Managarm is likely to diverge from other projects. Please read them carefully before preparing a pull request.

- Data types are `CamelCase`.
- Variable names are `camelCase`.
- Private members use a trailing underscore, like `privateMember_`.
- Indentation uses tabs.
- Braces are placed similarly to existing code.
- Single line bodies of `if`, `while`, `for` all go to a seperate line, like
```c
if(expression) {
	one-line statement;
}
```
- Single line bodies of `if`, `while`, `for` should use braces for readability.
- All headers have include guards. We use `#pragma once` for all headers with the exception of public headers in `mlibc`, which uses the classical include guards.
- Preferred line limit is 100 characters.

## Additional Guidelines

The coding style guidelines in this section are mostly well-established; we list them for completeness and to assist new contributors.

### Entity Naming

- Constants shall begin with an upper case letter, shall be upper case and words shall be separated by an underscore, like so `THIS_IS_A_CONSTANT`.
- Macros shall begin with an upper case letter, shall be upper case and words shall be separated by an underscore, like so `THIS_IS_A_MACRO`.

### Names

- Use sensible, descriptive names.
- Only use English names.
- Variables with a large scope shall have long names, variables with a small scope can have short names.
- Use namespaces for identifiers declared in different modules.
- Use name prefixes for identifiers declared in different modules.

### Indentation and Spacing

- Each statement shall be placed on a line on its own.
- Pointers and references shall be declared like `T *x` and `T &x`.
- The `const` keyword shall be used like `const int`.
- In a `switch` statement, cases shall be indented relative to the `switch` statement.

Example:
```c
switch(condition) {
	case 1:
		code;
		break;
	case 2:
		code;
		break;
	default:
		break;
}
```
- `Classes` and their access modifiers shall be indented on the same level.
- A `struct` is preferred over `class X { public`.
- All binary arithmetic, bitwise and assignment operators and the ternary conditional operator (?:) shall be surrounded by spaces; the comma operator shall be followed by a space but not preceded; all other operators shall not be used with spaces.
- Braces shall follow "K&R Bracing Style variant One True Bracing".
The K&R Bracing Style was first introduced in The C Programming Language by Brian Kernighan and Dennis Ritchie.

The opening brace is placed at the end of the enclosing statement and the closing brace is on a line on its own lined up with the enclosing statement. Statements and declaration between the braces are indented relative to the enclosing statement.

Example:
```c
int main(int argc, char *argv[]) {
	printf("Hello!\n");
	for(int i = 0; i < 10; i++ ) {
		printf("The value of i: %d\n", i);
		if(i == 3) {
			printf("Hey, i is 3!\n");
		}
	}
}
```

### Comments

- Comments shall be written in english
- If the purpose of a class or function cannot easily be recognized from its name, the class or function shall be preceded by a comment describing its purpose.
- Multiline comments shall use the C-style. Be consistent and use the `/* ... */` style for multiline comments.
- Single line comments shall use the C++-style. Be consistent and use the `// ...` style for single line comments.
- Use JavaDoc style comments for Doxygen. The comment styles `///` and `/** ... */` are used by JavaDoc, Doxygen and some other code documenting tools.
- All comments shall be placed above the line the comment describes, indented identically.

### Files

- File name shall be treated as case sensitive.
- File names shall be lower case, shall not contain spaces and shall use dashes (`-`) and not underscores as a word seperator.
- C source files shall have extension ".c".
- C header files shall have extension ".h".
- C++ source files shall have extension ".cpp".
- C++ header files shall have extension ".hpp".
- Header files must have include guards.
The include guard protects against the header file being included multiple times. We use `#pragma once` for all headers. Exception: public headers in `mlibc` have include guards.
- The name of the macro used in the include guard shall have the same name as the file (excluding the extension) followed by the suffix "_H".
- All headers shall be included with <>
Exception: In programs or libraries that consist of only a single source directory, private headers shall be included with ""
- Put #include directives at the top of files.
Having all #include directives in one place makes it easy to find them. 

### Declarations

- Declare class data private.
Classes shall encapsulate their data and only provide access to this data by member functions to ensure that data in class objects are consistent.

The exception to the rule is C type struct that only contains data members.

### Statements

- Use gotos only in special circumstances.
One possible use of gotos are try-again loops where `while` or `for` loops would obscure the structure of the code.

Example:
```
tryAgain:
    [...]
    if(!foo.compare_exchange(/* ... */))
        goto tryAgain;
```

- All switch statements shall have a default label.
Even if there is no action for the default label, it shall be included to show that the programmer has considered values not covered by case labels. If the case labels cover all possibilities, it is useful to put an assertion there to document the fact that it is impossible to get here. An assertion also protects from a future situation where a new possibility is introduced by mistake.

### Semantics

- Use plenty of assertions.
Assertions are useful to verify pre-conditions, post-conditions and any other conditions that should never happen. Pre-conditions are useful to verify that functions are called with valid arguments. They are also useful as documentation of what argument value ranges a function is designed to work with. However, we do not use assertions for user controlled input.

Assertions are macros that print error messages when the condition is not met. The macros are disabled in release mode and do not cost anything in performance or used memory in the end product.

Example: This square root function is only designed to work with positive numbers. 
```c
#include <assert.h>

double sqrt(double x) {
    // precondition: x is positive
    assert(x > 0);
    double result;
    ...
    // postcondition: result^2 ~= x
    assert(abs(result * result - x) / x < 1E-8) ;
}
```

## Attribution
This guide is based on the **Coding Standard Generator**. [Make your own](http://www.rosvall.ie/CSG/)!