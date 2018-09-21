#pragma once
class RecipeFactory
{
public:
	RecipeFactory();
	~RecipeFactory();

	void Reset();
	void Initialize();
	void UpdateCraftTableData();
	void UpdateExitingRecipes();


private:
	PackableHashTableWithJson<DWORD, JsonCraftOperation> *_recipesFromJson = NULL;
	PackableListWithJson<CraftPrecursor> _jsonPrecursorMap;
	PackableListWithJson<JsonCraftOperation> _jsonRecipes;

	bool RecipeInJson(DWORD recipeid, JsonCraftOperation* recipe);
	CCraftOperation GetCraftOpertionFromNewRecipe(JsonCraftOperation* recipe);
};

