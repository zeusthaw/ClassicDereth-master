#include "StdAfx.h"
#include "PhatSDK.h"
#include "RandomRange.h"
#include "Random.h"
#include <random>

std::random_device randomDevice;
CSharpRandom rng = CSharpRandom(randomDevice());

//std::mt19937 randomGenerator(randomDevice());
//
//int GenerateRandomInt(int minInclusive, int maxInclusive)
//{
//	std::uniform_int_distribution<int> randomDistribution(minInclusive, maxInclusive);
//	return randomDistribution(randomGenerator);
//}
//
//double GenerateRandomDouble(double minInclusive, double maxInclusive)
//{
//	std::uniform_real_distribution<double> randomDistribution(minInclusive, maxInclusive);
//	return randomDistribution(randomGenerator);
//}

void testRandomValueGenerator()
{
	int testRolls = 10000;
	std::map<int, int> valueDistribution;
	for (int i = 0; i < testRolls; i++)
	{
		//int test = (int)floor(getRandomDouble(0.0, 1.0, eRandomFormula::favorMid, 2, 0) * 10);
		int test = getRandomNumber(1, 10, eRandomFormula::favorSpecificValue, 3, 0, 4);
		//int test = getRandomNumber(0, 10, eRandomFormula::favorMid, 2);
		if (valueDistribution.find(test) != valueDistribution.end())
			valueDistribution[test]++;
		else
			valueDistribution.emplace(test, 1);
	}

	for each(auto entry in valueDistribution)
	{
		LOG(Data, Error, "value: %d amount: %d percent: %f\n", entry.first, entry.second, entry.second * 100.0 / testRolls);
	}
}

int getRandomNumberWithFavoredValue(int minInclusive, int maxInclusive, double favorValue, double favorStrength)
{
	//-----Merged from GDLE2 https://gitlab.com/Scribble/gdlenhanced/commit/8e41110b5c1db340b70acfd30a94a5ffecc2fcda ------//
	// Okay, so here goes nothing!
	// 1/a^abs(x-m) is a function that grows exponentially towards m either side of it on the x-axis
	// where a = 1 + favorStrength^2/(maxInclusive-minInclusive)
	// we want to randomly sample a random number in the area of this function to get a biased value
	// between n and m (n<m), the area is a^(n-m)/log(a)
	// between m and n (n>m), the area is (2-a^m-n)/log(a) where m is the favorValue

	int numValues = (maxInclusive - minInclusive) + 1;

	double a = 1 + pow(favorStrength, 2) / numValues;
	double logA = log(a);

	// we assume that each point has a width of 1 (+-0.5)
	// so we pick a random number between the area left of (minInclusive-0.5) so:
	double minArea = pow(a, (minInclusive - 0.5) - favorValue) / logA;
	// and the area left of (maxInclusive+0.5)
	double totArea = (2 - pow(a, favorValue - (maxInclusive + 0.5))) / logA - minArea;
	// here goes...
	double r = (rand() / (long double)RAND_MAX) * totArea + minArea;

	// the area at n=m is a^(n-m)/log(a) = a^0/log(a) = 1/log(a)
	if (r < 1 / logA)
	{
		// Now we just have to reaarange the equation for n
		// so n such that a^(n-m)/log(a) = r our randomly picked area
		return min(max(minInclusive, round(log(r*logA) / logA + favorValue)), maxInclusive);
	}
	else

	{
		// similarly,
		// n such that (2 - a^(m-n))/log(a) = r
		return min(max(minInclusive, round(favorValue - log(2 - r * logA) / logA)), maxInclusive);

	}

	// the equations chosen are simply what was here before, but calculated with a bit more elegance...
}

int getRandomNumberExclusive(int maxExclusive)
{
	return getRandomNumber(0, maxExclusive - 1, eRandomFormula::equalDistribution, 0, 0, 0);
}

int getRandomNumberExclusive(int maxExclusive, eRandomFormula formula, double favorStrength, double favorModifier, double favorSpecificValue)
{
	return getRandomNumber(0, maxExclusive - 1, formula, favorSpecificValue, favorStrength, favorModifier);
}

int getRandomNumber(int maxInclusive)
{
	return getRandomNumber(0, maxInclusive, eRandomFormula::equalDistribution, 0, 0, 0);
}

int getRandomNumber(int minInclusive, int maxInclusive)
{
	return getRandomNumber(minInclusive, maxInclusive, eRandomFormula::equalDistribution, 0, 0, 0);
}

int getRandomNumber(int minInclusive, int maxInclusive, eRandomFormula formula, double favorStrength, double favorModifier, double favorSpecificValue)
{
	int numbersAmount = maxInclusive - minInclusive;
	switch (formula)
	{
	case eRandomFormula::favorSpecificValue:
	{
		favorSpecificValue = favorSpecificValue + (numbersAmount * favorModifier);
		favorSpecificValue = min(favorSpecificValue, maxInclusive);
		favorSpecificValue = max(favorSpecificValue, minInclusive);
		return getRandomNumberWithFavoredValue(minInclusive, maxInclusive, favorSpecificValue, favorStrength);
	}
	case eRandomFormula::favorLow:
	{
		favorSpecificValue = minInclusive + (numbersAmount * favorModifier);
		favorSpecificValue = min(favorSpecificValue, maxInclusive);
		favorSpecificValue = max(favorSpecificValue, minInclusive);
		return getRandomNumberWithFavoredValue(minInclusive, maxInclusive, favorSpecificValue, favorStrength);
	}
	case eRandomFormula::favorMid:
	{
		int midValue = (int)round(((double)(maxInclusive - minInclusive) / 2)) + minInclusive;
		favorSpecificValue = midValue + (numbersAmount * favorModifier);
		favorSpecificValue = min(favorSpecificValue, maxInclusive);
		favorSpecificValue = max(favorSpecificValue, minInclusive);
		return getRandomNumberWithFavoredValue(minInclusive, maxInclusive, favorSpecificValue, favorStrength);
	}
	case eRandomFormula::favorHigh:
	{
		favorSpecificValue = maxInclusive - (numbersAmount * favorModifier);
		favorSpecificValue = min(favorSpecificValue, maxInclusive);
		favorSpecificValue = max(favorSpecificValue, minInclusive);
		return getRandomNumberWithFavoredValue(minInclusive, maxInclusive, favorSpecificValue, favorStrength);
	}
	default:
	case eRandomFormula::equalDistribution:
	{
		return rng.Next(minInclusive, maxInclusive + 1);
		//return Random::GenInt(minInclusive, maxInclusive);
	}
	}
}

std::set<int> getRandomNumbersNoRepeat(int amount, int minInclusive, int maxInclusive)
{
	std::set<int> numbers;
	for (int i = 0; i < amount; i++)
	{
		numbers.emplace(getRandomNumberNoRepeat(minInclusive, maxInclusive, numbers));
	}
	return numbers;
}

int getRandomNumberNoRepeat(int minInclusive, int maxInclusive, std::set<int> notThese, int maxTries)
{
	int potentialValue = getRandomNumber(minInclusive, maxInclusive, eRandomFormula::equalDistribution, 0, 0, 0);
	for (int i = 0; i < maxTries; i++)
	{
		potentialValue = getRandomNumber(minInclusive, maxInclusive, eRandomFormula::equalDistribution, 0, 0, 0);
		if (notThese.find(potentialValue) == notThese.end())
			break;
	}
	return potentialValue;
}

double getRandomDouble(double maxInclusive)
{
	return getRandomDouble(0, maxInclusive, eRandomFormula::equalDistribution, 0, 0, 0);
}

double getRandomDouble(double maxInclusive, eRandomFormula formula, double favorStrength, double favorModifier, double favorSpecificValue)
{
	return getRandomDouble(0, maxInclusive, formula, favorStrength, favorModifier, favorSpecificValue);
}

double getRandomDouble(double minInclusive, double maxInclusive)
{
	return getRandomDouble(minInclusive, maxInclusive, eRandomFormula::equalDistribution, 0, 0, 0);
}

double getRandomDouble(double minInclusive, double maxInclusive, eRandomFormula formula, double favorStrength, double favorModifier, double favorSpecificValue)
{
	double decimalPlaces = 1000;
	int minInt = (int)round(minInclusive * decimalPlaces);
	int maxInt = (int)round(maxInclusive * decimalPlaces);

	int favorSpecificValueInt = (int)round(favorSpecificValue * decimalPlaces);

	int randomInt = getRandomNumber(minInt, maxInt, formula, favorStrength, favorModifier, favorSpecificValue);
	double returnValue = randomInt / decimalPlaces;

	returnValue = min(returnValue, maxInclusive);
	returnValue = max(returnValue, minInclusive);

	return returnValue;
}
